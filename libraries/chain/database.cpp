#include <openssl/md5.h>

#include <boost/iostreams/device/mapped_file.hpp>

#include <graphene/protocol/chain_operations.hpp>

#include <graphene/chain/block_summary_object.hpp>
#include <graphene/chain/compound.hpp>
#include <graphene/chain/custom_operation_interpreter.hpp>
#include <graphene/chain/database.hpp>
#include <graphene/chain/database_exceptions.hpp>
#include <graphene/chain/db_with.hpp>
#include <graphene/chain/evaluator_registry.hpp>
#include <graphene/chain/index.hpp>
#include <graphene/chain/chain_evaluator.hpp>
#include <graphene/chain/chain_objects.hpp>
#include <graphene/chain/transaction_object.hpp>
#include <graphene/chain/shared_db_merkle.hpp>
#include <graphene/chain/operation_notification.hpp>
#include <graphene/chain/proposal_object.hpp>
#include <graphene/chain/committee_objects.hpp>
#include <graphene/chain/invite_objects.hpp>

#include <fc/smart_ref_impl.hpp>

#include <fc/container/deque.hpp>

#include <fc/io/fstream.hpp>
#include <fc/io/json.hpp>

#include <appbase/application.hpp>
#include <csignal>
#include <cerrno>
#include <cstring>

#define VIRTUAL_SCHEDULE_LAP_LENGTH  ( fc::uint128_t(uint64_t(-1)) )
#define VIRTUAL_SCHEDULE_LAP_LENGTH2 ( fc::uint128_t::max_value() )

namespace graphene { namespace chain {

struct object_schema_repr {
    std::pair<uint16_t, uint16_t> space_type;
    std::string type;
};

struct operation_schema_repr {
    std::string id;
    std::string type;
};

struct db_schema {
    std::map<std::string, std::string> types;
    std::vector<object_schema_repr> object_types;
    std::string operation_type;
    std::vector<operation_schema_repr> custom_operation_types;
};

struct snapshot_account
{
    std::string login;
    std::string public_key;//authority
    uint64_t shares_ammount;
};

struct snapshot_items
{
    vector <snapshot_account> accounts;
};

} } // graphene::chain

FC_REFLECT((graphene::chain::object_schema_repr), (space_type)(type))
FC_REFLECT((graphene::chain::operation_schema_repr), (id)(type))
FC_REFLECT((graphene::chain::db_schema), (types)(object_types)(operation_type)(custom_operation_types))
FC_REFLECT((graphene::chain::snapshot_account), (login)(public_key)(shares_ammount))
FC_REFLECT((graphene::chain::snapshot_items), (accounts))



namespace graphene { namespace chain {

        using std::sig_atomic_t;
        using boost::container::flat_set;

        inline u256 to256(const fc::uint128_t &t) {
            u256 v(t.hi);
            v <<= 64;
            v += t.lo;
            return v;
        }

        class signal_guard {
            struct sigaction old_hup_action, old_int_action, old_term_action;

            static volatile sig_atomic_t is_interrupted;

            bool is_restored = true;

            static inline void throw_exception();

            public:
                inline signal_guard();

                inline ~signal_guard();

                void setup();

                void restore();

                static inline sig_atomic_t get_is_interrupted() noexcept;
        };

        volatile sig_atomic_t signal_guard::is_interrupted = false;

        inline void signal_guard::throw_exception() {
            FC_THROW_EXCEPTION(database_signal_exception,
                               "Error setup signal handler: ${e}.",
                               ("e", strerror(errno)));
        }

        inline signal_guard::signal_guard() {
            setup();
        }

        inline signal_guard::~signal_guard() {
            try {
                restore();
            }
            FC_CAPTURE_AND_LOG(())
        }

        void signal_guard::setup() {
            if (is_restored) {
                struct sigaction new_action;

                new_action.sa_handler = [](int signum) {
                    is_interrupted = true;
                };
                new_action.sa_flags = SA_RESTART;

                if (sigemptyset(&new_action.sa_mask) == -1) {
                    throw_exception();
                }

                if (sigaction(SIGHUP, &new_action, &old_hup_action) == -1) {
                    throw_exception();
                }

                if (sigaction(SIGINT, &new_action, &old_int_action) == -1) {
                    throw_exception();
                }

                if (sigaction(SIGTERM, &new_action, &old_term_action) == -1) {
                    throw_exception();
                }

                is_restored = false;
            }
        }

        void signal_guard::restore() {
            if (!is_restored) {
                if (sigaction(SIGHUP, &old_hup_action, nullptr) == -1) {
                    throw_exception();
                }

                if (sigaction(SIGINT, &old_int_action, nullptr) == -1) {
                    throw_exception();
                }

                if (sigaction(SIGTERM, &old_term_action, nullptr) == -1) {
                    throw_exception();
                }

                is_interrupted = false;
                is_restored = true;
            }
        }

        inline sig_atomic_t signal_guard::get_is_interrupted() noexcept {
            return is_interrupted;
        }

        class database_impl {
        public:
            database_impl(database &self);

            database &_self;
            evaluator_registry<operation> _evaluator_registry;
        };

        database_impl::database_impl(database &self)
                : _self(self), _evaluator_registry(self) {
        }

        database::database()
                : _my(new database_impl(*this)) {
        }

        database::~database() {
            clear_pending();
        }

        void database::open(const fc::path &data_dir, const fc::path &shared_mem_dir, uint64_t initial_supply, uint64_t shared_file_size, uint32_t chainbase_flags) {
            try {
                auto start = fc::time_point::now();
                wlog("Start opening database. Please wait, don't break application...");

                init_schema();
                chainbase::database::open(shared_mem_dir, chainbase_flags, shared_file_size);

                initialize_indexes();
                initialize_evaluators();

                auto end = fc::time_point::now();
                wlog("Done opening database, elapsed time ${t} sec", ("t", double((end - start).count()) / 1000000.0));

                if (chainbase_flags & chainbase::database::read_write) {
                    start = fc::time_point::now();
                    wlog("Start opening block log. Please wait, don't break application...");

                    if (!find<dynamic_global_property_object>()) {
                        with_strong_write_lock([&]() {
                            init_genesis(initial_supply);
                        });
                    }

                    _block_log.open(data_dir / "block_log");

                    // Rewind all undo state. This should return us to the state at the last irreversible block.
                    with_strong_write_lock([&]() {
                        undo_all();
                    });

                    if (revision() != head_block_num()) {
                        with_strong_read_lock([&]() {
                            init_hardforks(); // Writes to local state, but reads from db
                        });

                        FC_THROW_EXCEPTION(database_revision_exception,
                                           "Chainbase revision does not match head block num, "
                                           "current revision is ${rev}, "
                                           "current head block num is ${head}",
                                           ("rev", revision())
                                           ("head", head_block_num()));
                    }

                    if (head_block_num()) {
                        auto head_block = _block_log.read_block_by_num(head_block_num());
                        // This assertion should be caught and a reindex should occur
                        FC_ASSERT(head_block.valid() && head_block->id() ==
                                                        head_block_id(), "Chain state does not match block log. Please reindex blockchain.");

                        _fork_db.start_block(*head_block);
                    }
                    end = fc::time_point::now();
                    wlog("Done opening block log, elapsed time ${t} sec", ("t", double((end - start).count()) / 1000000.0));
                }

                with_strong_read_lock([&]() {
                    init_hardforks(); // Writes to local state, but reads from db
                });

            }
            FC_CAPTURE_LOG_AND_RETHROW((data_dir)(shared_mem_dir)(shared_file_size))
        }

        void database::reindex(const fc::path &data_dir, const fc::path &shared_mem_dir, uint32_t from_block_num, uint64_t shared_file_size) {
            try {
                signal_guard sg;
                _fork_db.reset();    // override effect of _fork_db.start_block() call in open()

                auto start = fc::time_point::now();
                CHAIN_ASSERT(_block_log.head(), block_log_exception, "No blocks in block log. Cannot reindex an empty chain.");

                ilog("Replaying blocks...");

                uint64_t skip_flags =
                        skip_block_size_check |
                        skip_witness_signature |
                        skip_transaction_signatures |
                        skip_transaction_dupe_check |
                        skip_tapos_check |
                        skip_merkle_check |
                        skip_witness_schedule_check |
                        skip_authority_check |
                        skip_validate_operations | /// no need to validate operations
                        skip_block_log;

                with_strong_write_lock([&]() {
                    auto cur_block_num = from_block_num;
                    auto last_block_num = _block_log.head()->block_num();
                    auto last_block_pos = _block_log.get_block_pos(last_block_num);
                    int last_reindex_percent = 0;

                    set_reserved_memory(1024*1024*1024); // protect from memory fragmentations ...
                    while (cur_block_num < last_block_num) {
                        if (signal_guard::get_is_interrupted()) {
                            return;
                        }

                        auto end = fc::time_point::now();
                        auto cur_block_pos = _block_log.get_block_pos(cur_block_num);
                        auto cur_block = *_block_log.read_block_by_num(cur_block_num);

                        auto reindex_percent = cur_block_pos * 100 / last_block_pos;
                        if (reindex_percent - last_reindex_percent >= 1) {
                            std::cerr
                                << "   " << reindex_percent << "%   "
                                << cur_block_num << " of " << last_block_num
                                << "   ("  << (free_memory() / (1024 * 1024)) << "M free"
                                << ", elapsed " << double((end - start).count()) / 1000000.0 << " sec)\n";

                            last_reindex_percent = reindex_percent;
                        }

                        apply_block(cur_block, skip_flags);

                        if (cur_block_num % 1000 == 0) {
                            set_revision(head_block_num());
                        }

                        check_free_memory(true, cur_block_num);
                        cur_block_num++;
                    }

                    auto cur_block = *_block_log.read_block_by_num(cur_block_num);
                    apply_block(cur_block, skip_flags);
                    set_reserved_memory(0);
                    set_revision(head_block_num());
                });

                if (signal_guard::get_is_interrupted()) {
                    sg.restore();

                    appbase::app().quit();
                }

                if (_block_log.head()->block_num()) {
                    _fork_db.start_block(*_block_log.head());
                }
                auto end = fc::time_point::now();
                ilog("Done reindexing, elapsed time: ${t} sec", ("t",
                        double((end - start).count()) / 1000000.0));
            }
            FC_CAPTURE_AND_RETHROW((data_dir)(shared_mem_dir))

        }

        void database::set_min_free_shared_memory_size(size_t value) {
            _min_free_shared_memory_size = value;
        }

        void database::set_inc_shared_memory_size(size_t value) {
            _inc_shared_memory_size = value;
        }

        void database::set_block_num_check_free_size(uint32_t value) {
            _block_num_check_free_memory = value;
        }

        void database::set_skip_virtual_ops() {
            _skip_virtual_ops = true;
        }

        bool database::_resize(uint32_t current_block_num) {
            if (_inc_shared_memory_size == 0) {
                elog("Auto-scaling of shared file size is not configured!. Do it immediately!");
                return false;
            }

            uint64_t max_mem = max_memory();

            size_t new_max = max_mem + _inc_shared_memory_size;
            wlog(
                "Memory is almost full on block ${block}, increasing to ${mem}M",
                ("block", current_block_num)("mem", new_max / (1024 * 1024)));
            resize(new_max);

            uint64_t free_mem = free_memory();
            uint64_t reserved_mem = reserved_memory();

            if (free_mem > reserved_mem) {
                free_mem -= reserved_mem;
            }

            uint32_t free_mb = uint32_t(free_mem / (1024 * 1024));
            uint32_t reserved_mb = uint32_t(reserved_mem / (1024 * 1024));
            wlog("Free memory is now ${free}M (${reserved}M)", ("free", free_mb)("reserved", reserved_mb));
            _last_free_gb_printed = free_mb / 1024;
            return true;
        }

        void database::check_free_memory(bool skip_print, uint32_t current_block_num) {
            if (0 != current_block_num % _block_num_check_free_memory) {
                return;
            }

            uint64_t reserved_mem = reserved_memory();
            uint64_t free_mem = free_memory();

            if (free_mem > reserved_mem) {
                free_mem -= reserved_mem;
            } else {
                set_reserved_memory(0);
            }

            if (_inc_shared_memory_size != 0 && _min_free_shared_memory_size != 0 &&
                free_mem < _min_free_shared_memory_size
            ) {
                _resize(current_block_num);
            } else if (!skip_print && _inc_shared_memory_size == 0 && _min_free_shared_memory_size == 0) {
                uint32_t free_gb = uint32_t(free_mem / (1024 * 1024 * 1024));
                if ((free_gb < _last_free_gb_printed) || (free_gb > _last_free_gb_printed + 1)) {
                    ilog(
                        "Free memory is now ${n}G. Current block number: ${block}",
                        ("n", free_gb)("block", current_block_num));
                    _last_free_gb_printed = free_gb;
                }

                if (free_gb == 0) {
                    uint32_t free_mb = uint32_t(free_mem / (1024 * 1024));
                    if (free_mb <= 500 && current_block_num % 10 == 0) {
                        elog("Free memory is now ${n}M. Increase shared file size immediately!", ("n", free_mb));
                    }
                }
            }
        }

        void database::wipe(const fc::path &data_dir, const fc::path &shared_mem_dir, bool include_blocks) {
            close();
            chainbase::database::wipe(shared_mem_dir);
            if (include_blocks) {
                fc::remove_all(data_dir / "block_log");
                fc::remove_all(data_dir / "block_log.index");
            }
        }

        void database::close(bool rewind) {
            try {
                // Since pop_block() will move tx's in the popped blocks into pending,
                // we have to clear_pending() after we're done popping to get a clean
                // DB state (issue #336).
                clear_pending();

                chainbase::database::flush();
                chainbase::database::close();

                _block_log.close();

                _fork_db.reset();
            }
            FC_CAPTURE_AND_RETHROW()
        }

        bool database::is_known_block(const block_id_type &id) const {
            try {
                return fetch_block_by_id(id).valid();
            } FC_CAPTURE_AND_RETHROW()
        }

/**
 * Only return true *if* the transaction has not expired or been invalidated. If this
 * method is called with a VERY old transaction we will return false, they should
 * query things by blocks if they are that old.
 */
        bool database::is_known_transaction(const transaction_id_type &id) const {
            try {
                const auto &trx_idx = get_index<transaction_index>().indices().get<by_trx_id>();
                return trx_idx.find(id) != trx_idx.end();
            } FC_CAPTURE_AND_RETHROW()
        }

        block_id_type database::find_block_id_for_num(uint32_t block_num) const {
            try {
                if (block_num == 0) {
                    return block_id_type();
                }

                // Reversible blocks are *usually* in the TAPOS buffer. Since this
                // is the fastest check, we do it first.
                block_summary_id_type bsid = block_num & 0xFFFF;
                const block_summary_object *bs = find<block_summary_object, by_id>(bsid);
                if (bs != nullptr) {
                    if (protocol::block_header::num_from_id(bs->block_id) ==
                        block_num) {
                        return bs->block_id;
                    }
                }

                // Next we query the block log. Irreversible blocks are here.

                auto b = _block_log.read_block_by_num(block_num);
                if (b.valid()) {
                    return b->id();
                }

                // Finally we query the fork DB.
                shared_ptr<fork_item> fitem = _fork_db.fetch_block_on_main_branch_by_number(block_num);
                if (fitem) {
                    return fitem->id;
                }

                return block_id_type();
            }
            FC_CAPTURE_AND_RETHROW((block_num))
        }

        block_id_type database::get_block_id_for_num(uint32_t block_num) const {
            block_id_type bid = find_block_id_for_num(block_num);
            FC_ASSERT(bid != block_id_type());
            return bid;
        }

        optional<signed_block> database::fetch_block_by_id(const block_id_type &id) const {
            try {
                auto b = _fork_db.fetch_block(id);
                if (!b) {
                    auto tmp = _block_log.read_block_by_num(protocol::block_header::num_from_id(id));

                    if (tmp && tmp->id() == id) {
                        return tmp;
                    }

                    tmp.reset();
                    return tmp;
                }

                return b->data;
            } FC_CAPTURE_AND_RETHROW()
        }

        optional<signed_block> database::fetch_block_by_number(uint32_t block_num) const {
            try {
                optional<signed_block> b;

                auto results = _fork_db.fetch_block_by_number(block_num);
                if (results.size() == 1) {
                    b = results[0]->data;
                } else {
                    b = _block_log.read_block_by_num(block_num);
                }

                return b;
            } FC_LOG_AND_RETHROW()
        }

        const signed_transaction database::get_recent_transaction(const transaction_id_type &trx_id) const {
            try {
                auto &index = get_index<transaction_index>().indices().get<by_trx_id>();
                auto itr = index.find(trx_id);
                FC_ASSERT(itr != index.end());
                signed_transaction trx;
                fc::raw::unpack(itr->packed_trx, trx);
                return trx;;
            } FC_CAPTURE_AND_RETHROW()
        }

        std::vector<block_id_type> database::get_block_ids_on_fork(block_id_type head_of_fork) const {
            try {
                pair<fork_database::branch_type, fork_database::branch_type> branches = _fork_db.fetch_branch_from(head_block_id(), head_of_fork);
                if (!((branches.first.back()->previous_id() ==
                       branches.second.back()->previous_id()))) {
                    edump((head_of_fork)
                            (head_block_id())
                            (branches.first.size())
                            (branches.second.size()));
                    assert(branches.first.back()->previous_id() ==
                           branches.second.back()->previous_id());
                }
                std::vector<block_id_type> result;
                for (const item_ptr &fork_block : branches.second) {
                    result.emplace_back(fork_block->id);
                }
                result.emplace_back(branches.first.back()->previous_id());
                return result;
            } FC_CAPTURE_AND_RETHROW()
        }

        chain_id_type database::get_chain_id() const {
            return CHAIN_ID;
        }

        const witness_object &database::get_witness(const account_name_type &name) const {
            try {
                return get<witness_object, by_name>(name);
            } FC_CAPTURE_AND_RETHROW((name))
        }

        const witness_object *database::find_witness(const account_name_type &name) const {
            return find<witness_object, by_name>(name);
        }

        const account_object &database::get_account(const account_name_type &name) const {
            try {
                return get<account_object, by_name>(name);
            } FC_CAPTURE_AND_RETHROW((name))
        }

        const account_object *database::find_account(const account_name_type &name) const {
            return find<account_object, by_name>(name);
        }

        const content_object &database::get_content(const account_name_type &author, const shared_string &permlink) const {
            try {
                return get<content_object, by_permlink>(boost::make_tuple(author, permlink));
            } FC_CAPTURE_AND_RETHROW((author)(permlink))
        }

        const content_object *database::find_content(const account_name_type &author, const shared_string &permlink) const {
            return find<content_object, by_permlink>(boost::make_tuple(author, permlink));
        }

        const content_object &database::get_content(const account_name_type &author, const string &permlink) const {
            try {
                return get<content_object, by_permlink>(boost::make_tuple(author, permlink));
            } FC_CAPTURE_AND_RETHROW((author)(permlink))
        }

        const content_object *database::find_content(const account_name_type &author, const string &permlink) const {
            return find<content_object, by_permlink>(boost::make_tuple(author, permlink));
        }


        const content_type_object &database::get_content_type(const content_id_type &content) const {
            try {
                return get<content_type_object, by_content>(content);
            } FC_CAPTURE_AND_RETHROW((content))
        }

        const content_type_object *database::find_content_type(const content_id_type &content) const {
            return find<content_type_object, by_content>(content);
        }

        const escrow_object &database::get_escrow(const account_name_type &name, uint32_t escrow_id) const {
            try {
                return get<escrow_object, by_from_id>(boost::make_tuple(name, escrow_id));
            } FC_CAPTURE_AND_RETHROW((name)(escrow_id))
        }

        const escrow_object *database::find_escrow(const account_name_type &name, uint32_t escrow_id) const {
            return find<escrow_object, by_from_id>(boost::make_tuple(name, escrow_id));
        }

        const dynamic_global_property_object &database::get_dynamic_global_properties() const {
            try {
                return get<dynamic_global_property_object>();
            } FC_CAPTURE_AND_RETHROW()
        }

        const witness_schedule_object &database::get_witness_schedule_object() const {
            try {
                return get<witness_schedule_object>();
            } FC_CAPTURE_AND_RETHROW()
        }

        const hardfork_property_object &database::get_hardfork_property_object() const {
            try {
                return get<hardfork_property_object>();
            } FC_CAPTURE_AND_RETHROW()
        }

        const time_point_sec database::calculate_discussion_payout_time(const content_object &content) const {
            return content.cashout_time;
        }

        bool database::update_account_bandwidth(const account_object &a, uint32_t trx_size) {
            const auto &props = get_dynamic_global_properties();
            bool has_bandwidth = true;
            const witness_schedule_object &consensus = get_witness_schedule_object();

            if (props.total_vesting_shares.amount > 0) {
                share_type new_bandwidth;
                share_type trx_bandwidth = trx_size * CHAIN_BANDWIDTH_PRECISION;
                auto delta_time = (head_block_time() - a.last_bandwidth_update).to_seconds();
                if (delta_time > CHAIN_BANDWIDTH_AVERAGE_WINDOW_SECONDS) {
                    new_bandwidth = 0;
                } else {
                    new_bandwidth = (
                            ((CHAIN_BANDWIDTH_AVERAGE_WINDOW_SECONDS -
                              delta_time) *
                             fc::uint128_t(a.average_bandwidth.value))
                            /
                            CHAIN_BANDWIDTH_AVERAGE_WINDOW_SECONDS).to_uint64();
                }

                new_bandwidth += trx_bandwidth;
                modify(a, [&](account_object &acnt) {
                    acnt.average_bandwidth = new_bandwidth;
                    acnt.lifetime_bandwidth += trx_bandwidth;
                    acnt.last_bandwidth_update = head_block_time();
                });

                fc::uint128_t account_vshares(a.effective_vesting_shares().amount.value);
                fc::uint128_t total_vshares(props.total_vesting_shares.amount.value);
                fc::uint128_t account_average_bandwidth(a.average_bandwidth.value);
                fc::uint128_t max_virtual_bandwidth(props.max_virtual_bandwidth);

                if(account_vshares < consensus.median_props.bandwidth_reserve_below.amount.value){
                    account_vshares = total_vshares * consensus.median_props.bandwidth_reserve_percent / CHAIN_100_PERCENT / props.bandwidth_reserve_candidates;
                }
                else{
                    account_vshares = account_vshares * (CHAIN_100_PERCENT - consensus.median_props.bandwidth_reserve_percent) / CHAIN_100_PERCENT;
                }

                has_bandwidth = (account_vshares * max_virtual_bandwidth) > (account_average_bandwidth * total_vshares);

                if (is_producing())
                    FC_ASSERT(has_bandwidth, "Account exceeded maximum allowed bandwidth per vesting share.",
                        ("account_vshares", account_vshares)
                        ("account_average_bandwidth", account_average_bandwidth)
                        ("max_virtual_bandwidth", max_virtual_bandwidth)
                        ("total_vesting_shares", total_vshares));
            }

            return has_bandwidth;
        }

        uint32_t database::witness_participation_rate() const {
            const dynamic_global_property_object &dpo = get_dynamic_global_properties();
            return uint64_t(CHAIN_100_PERCENT) *
                   dpo.recent_slots_filled.popcount() / 128;
        }

        void database::add_checkpoints(const flat_map<uint32_t, block_id_type> &checkpts) {
            for (const auto &i : checkpts) {
                _checkpoints[i.first] = i.second;
            }
        }

        bool database::before_last_checkpoint() const {
            return (_checkpoints.size() > 0) &&
                   (_checkpoints.rbegin()->first >= head_block_num());
        }

        uint32_t database::validate_block(const signed_block& new_block, uint32_t skip) {
            uint32_t validate_block_steps =
                skip_merkle_check |
                skip_block_size_check;

            if ((skip & validate_block_steps) != validate_block_steps) {
                with_strong_read_lock([&](){
                    _validate_block(new_block, skip);
                });

                skip |= validate_block_steps;

                // it's tempting but very dangerous to check transaction signatures here
                //   because block can contain transactions with changing of authorizity
                //   and state can too contain changes of authorizity
            }

            return skip;
        }

        void database::_validate_block(const signed_block& new_block, uint32_t skip) {
            uint32_t new_block_num = new_block.block_num();

            if (!(skip & skip_merkle_check)) {
                auto merkle_root = new_block.calculate_merkle_root();

                try {
                    FC_ASSERT(
                        new_block.transaction_merkle_root == merkle_root,
                        "Merkle check failed",
                        ("next_block.transaction_merkle_root", new_block.transaction_merkle_root)
                        ("calc", merkle_root)
                        ("next_block", new_block)
                        ("id", new_block.id()));
                } catch (fc::assert_exception &e) {
                    const auto &merkle_map = get_shared_db_merkle();
                    auto itr = merkle_map.find(new_block_num);

                    if (itr == merkle_map.end() || itr->second != merkle_root) {
                        throw e;
                    }
                }
            }

            if (!(skip & skip_block_size_check)) {
                const auto &gprops = get_dynamic_global_properties();
                auto block_size = fc::raw::pack_size(new_block);
                FC_ASSERT(
                    block_size <= gprops.maximum_block_size,
                    "Block Size is too Big",
                    ("next_block_num", new_block_num)
                    ("block_size", block_size)
                    ("max", gprops.maximum_block_size));
            }
        }

       /**
        * Push block "may fail" in which case every partial change is unwound. After
        * push block is successful the block is appended to the chain database on disk.
        *
        * @return true if we switched forks as a result of this push.
        */
        bool database::push_block(const signed_block &new_block, uint32_t skip) {
            //fc::time_point begin_time = fc::time_point::now();

            bool result;
            with_strong_write_lock([&]() {
                detail::without_pending_transactions(*this, skip, std::move(_pending_tx), [&]() {
                    try {
                        result = _push_block(new_block, skip);
                        check_free_memory(false, new_block.block_num());
                    } catch (const fc::exception &e) {
                        auto msg = std::string(e.what());
                        // TODO: there is no easy way to catch boost::interprocess::bad_alloc
                        if (msg.find("boost::interprocess::bad_alloc") == msg.npos) {
                            throw e;
                        }
                        wlog("Receive bad_alloc exception. Forcing to resize shared memory file.");
                        set_reserved_memory(free_memory());
                        if (!_resize(new_block.block_num())) {
                            throw e;
                        }
                        result = _push_block(new_block, skip);
                    }
                });
            });

            //fc::time_point end_time = fc::time_point::now();
            //fc::microseconds dt = end_time - begin_time;
            //if( ( new_block.block_num() % 10000 ) == 0 )
            //   ilog( "push_block ${b} took ${t} microseconds", ("b", new_block.block_num())("t", dt.count()) );
            return result;
        }

        void database::_maybe_warn_multiple_production(uint32_t height) const {
            auto blocks = _fork_db.fetch_block_by_number(height);
            if (blocks.size() > 1) {
                vector<std::pair<account_name_type, fc::time_point_sec>> witness_time_pairs;
                for (const auto &b : blocks) {
                    witness_time_pairs.push_back(std::make_pair(b->data.witness, b->data.timestamp));
                }

                ilog(
                    "Encountered block num collision at block ${n} due to a fork, witnesses are: ${w}",
                    ("n", height)("w", witness_time_pairs));
            }
            return;
        }

        bool database::_push_block(const signed_block &new_block, uint32_t skip) {
            try {
                if (!(skip & skip_fork_db)) {
                    shared_ptr<fork_item> new_head = _fork_db.push_block(new_block);
                    _maybe_warn_multiple_production(new_head->num);
                    //If the head block from the longest chain does not build off of the current head, we need to switch forks.
                    if (new_head->data.previous != head_block_id()) {
                        //If the newly pushed block is the same height as head, we get head back in new_head
                        //Only switch forks if new_head is actually higher than head
                        if (new_head->data.block_num() > head_block_num()) {
                            // wlog( "Switching to fork: ${id}", ("id",new_head->data.id()) );
                            auto branches = _fork_db.fetch_branch_from(new_head->data.id(), head_block_id());

                            // pop blocks until we hit the forked block
                            while (head_block_id() !=
                                   branches.second.back()->data.previous) {
                                pop_block();
                            }

                            // push all blocks on the new fork
                            for (auto ritr = branches.first.rbegin();
                                 ritr != branches.first.rend(); ++ritr) {
                                // ilog( "pushing blocks from fork ${n} ${id}", ("n",(*ritr)->data.block_num())("id",(*ritr)->data.id()) );
                                optional<fc::exception> except;
                                try {
                                    auto session = start_undo_session();
                                    apply_block((*ritr)->data, skip);
                                    session.push();
                                }
                                catch (const fc::exception &e) {
                                    except = e;
                                }
                                if (except) {
                                    // wlog( "exception thrown while switching forks ${e}", ("e",except->to_detail_string() ) );
                                    // remove the rest of branches.first from the fork_db, those blocks are invalid
                                    while (ritr != branches.first.rend()) {
                                        _fork_db.remove((*ritr)->data.id());
                                        ++ritr;
                                    }
                                    _fork_db.set_head(branches.second.front());

                                    // pop all blocks from the bad fork
                                    while (head_block_id() !=
                                           branches.second.back()->data.previous) {
                                        pop_block();
                                    }

                                    // restore all blocks from the good fork
                                    for (auto ritr = branches.second.rbegin();
                                         ritr !=
                                         branches.second.rend(); ++ritr) {
                                        auto session = start_undo_session();
                                        apply_block((*ritr)->data, skip);
                                        session.push();
                                    }
                                    throw *except;
                                }
                            }
                            return true;
                        } else {
                            return false;
                        }
                    }
                }

                try {
                    auto session = start_undo_session();
                    apply_block(new_block, skip);
                    session.push();
                }
                catch (const fc::exception &e) {
                    elog("Failed to push new block:\n${e}", ("e", e.to_detail_string()));
                    _fork_db.remove(new_block.id());
                    throw;
                }

                return false;
            } FC_CAPTURE_AND_RETHROW()
        }

       /**
        * Attempts to push the transaction into the pending queue
        *
        * When called to push a locally generated transaction, set the skip_block_size_check bit on the skip argument. This
        * will allow the transaction to be pushed even if it causes the pending block size to exceed the maximum block size.
        * Although the transaction will probably not propagate further now, as the peers are likely to have their pending
        * queues full as well, it will be kept in the queue to be propagated later when a new block flushes out the pending
        * queues.
        */
        void database::push_transaction(const signed_transaction &trx, uint32_t skip) {
            try {
                FC_ASSERT(fc::raw::pack_size(trx) <= (get_dynamic_global_properties().maximum_block_size - 256));
                with_weak_write_lock([&]() {
                    detail::with_producing(*this, [&]() {
                        _push_transaction(trx, skip);
                    });
                });
            }
            FC_CAPTURE_AND_RETHROW((trx))
        }

        void database::_push_transaction(const signed_transaction &trx, uint32_t skip) {
            // If this is the first transaction pushed after applying a block, start a new undo session.
            // This allows us to quickly rewind to the clean state of the head block, in case a new block arrives.
            if (!_pending_tx_session.valid()) {
                _pending_tx_session = start_undo_session();
            }

            // Create a temporary undo session as a child of _pending_tx_session.
            // The temporary session will be discarded by the destructor if
            // _apply_transaction fails. If we make it to merge(), we
            // apply the changes.

            auto temp_session = start_undo_session();
            _apply_transaction(trx, skip);
            _pending_tx.push_back(trx);

            notify_changed_objects();
            // The transaction applied successfully. Merge its changes into the pending block session.
            temp_session.squash();

            // notify anyone listening to pending transactions
            notify_on_pending_transaction(trx);
        }

        signed_block database::generate_block(
                fc::time_point_sec when,
                const account_name_type &witness_owner,
                const fc::ecc::private_key &block_signing_private_key,
                uint32_t skip /* = 0 */
        ) {
            signed_block result;
            try {
                result = _generate_block(when, witness_owner, block_signing_private_key, skip);
            }
            FC_CAPTURE_AND_RETHROW((witness_owner))
            return result;
        }


        signed_block database::_generate_block(
                fc::time_point_sec when,
                const account_name_type &witness_owner,
                const fc::ecc::private_key &block_signing_private_key,
                uint32_t skip
        ) {
            uint32_t slot_num = get_slot_at_time(when);
            FC_ASSERT(slot_num > 0);
            string scheduled_witness = get_scheduled_witness(slot_num);
            FC_ASSERT(scheduled_witness == witness_owner);

            const auto &witness_obj = get_witness(witness_owner);

            if (!(skip & skip_witness_signature))
                FC_ASSERT(witness_obj.signing_key ==
                          block_signing_private_key.get_public_key());

            static const size_t max_block_header_size =
                    fc::raw::pack_size(signed_block_header()) + 4;
            auto maximum_block_size = get_dynamic_global_properties().maximum_block_size; //CHAIN_BLOCK_SIZE;
            size_t total_block_size = max_block_header_size;

            signed_block pending_block;

            with_strong_write_lock([&]() {
                //
                // The following code throws away existing pending_tx_session and
                // rebuilds it by re-applying pending transactions.
                //
                // This rebuild is necessary because pending transactions' validity
                // and semantics may have changed since they were received, because
                // time-based semantics are evaluated based on the current block
                // time. These changes can only be reflected in the database when
                // the value of the "when" variable is known, which means we need to
                // re-apply pending transactions in this method.
                //
                _pending_tx_session.reset();
                _pending_tx_session = start_undo_session();

                uint64_t postponed_tx_count = 0;
                // pop pending state (reset to head block state)
                for (const signed_transaction &tx : _pending_tx) {
                    // Only include transactions that have not expired yet for currently generating block,
                    // this should clear problem transactions and allow block production to continue

                    if (tx.expiration < when) {
                        continue;
                    }

                    uint64_t new_total_size =
                            total_block_size + fc::raw::pack_size(tx);

                    // postpone transaction if it would make block too big
                    if (new_total_size >= maximum_block_size) {
                        postponed_tx_count++;
                        continue;
                    }

                    try {
                        auto temp_session = start_undo_session();
                        _apply_transaction(tx, skip);
                        temp_session.squash();

                        total_block_size += fc::raw::pack_size(tx);
                        pending_block.transactions.push_back(tx);
                    }
                    catch (const fc::exception &e) {
                        // Do nothing, transaction will not be re-applied
                        //wlog( "Transaction was not processed while generating block due to ${e}", ("e", e) );
                        //wlog( "The transaction was ${t}", ("t", tx) );
                    }
                }
                if (postponed_tx_count > 0) {
                    wlog("Postponed ${n} transactions due to block size limit", ("n", postponed_tx_count));
                }

                _pending_tx_session.reset();
            });

            // We have temporarily broken the invariant that
            // _pending_tx_session is the result of applying _pending_tx, as
            // _pending_tx now consists of the set of postponed transactions.
            // However, the push_block() call below will re-create the
            // _pending_tx_session.

            pending_block.previous = head_block_id();
            pending_block.timestamp = when;
            pending_block.transaction_merkle_root = pending_block.calculate_merkle_root();
            pending_block.witness = witness_owner;

            const auto &witness = get_witness(witness_owner);

            if (witness.running_version != CHAIN_VERSION) {
                pending_block.extensions.insert(block_header_extensions(CHAIN_VERSION));
            }

            const auto &hfp = get_hardfork_property_object();

            if (hfp.current_hardfork_version <
                CHAIN_HARDFORK_VERSION // Binary is newer hardfork than has been applied
                && (witness.hardfork_version_vote !=
                    _hardfork_versions[hfp.last_hardfork + 1] ||
                    witness.hardfork_time_vote !=
                    _hardfork_times[hfp.last_hardfork +
                                    1])) // Witness vote does not match binary configuration
            {
                // Make vote match binary configuration
                pending_block.extensions.insert(block_header_extensions(hardfork_version_vote(_hardfork_versions[
                        hfp.last_hardfork + 1], _hardfork_times[
                        hfp.last_hardfork + 1])));
            } else if (hfp.current_hardfork_version ==
                       CHAIN_HARDFORK_VERSION // Binary does not know of a new hardfork
                       && witness.hardfork_version_vote >
                          CHAIN_HARDFORK_VERSION) // Voting for hardfork in the future, that we do not know of...
            {
                // Make vote match binary configuration. This is vote to not apply the new hardfork.
                pending_block.extensions.insert(block_header_extensions(hardfork_version_vote(_hardfork_versions[hfp.last_hardfork], _hardfork_times[hfp.last_hardfork])));
            }

            if (!(skip & skip_witness_signature)) {
                pending_block.sign(block_signing_private_key);
            }

            // TODO: Move this to _push_block() so session is restored.
            if (!(skip & skip_block_size_check)) {
                FC_ASSERT(fc::raw::pack_size(pending_block) <= CHAIN_BLOCK_SIZE);
            }

            push_block(pending_block, skip);

            return pending_block;
        }

/**
 * Removes the most recent block from the database and
 * undoes any changes it made.
 */
        void database::pop_block() {
            try {
                _pending_tx_session.reset();
                auto head_id = head_block_id();

                /// save the head block so we can recover its transactions
                optional<signed_block> head_block = fetch_block_by_id(head_id);
                CHAIN_ASSERT(head_block.valid(), pop_empty_chain, "there are no blocks to pop");

                _fork_db.pop_block();
                undo();

                _popped_tx.insert(_popped_tx.begin(), head_block->transactions.begin(), head_block->transactions.end());

            }
            FC_CAPTURE_AND_RETHROW()
        }

        void database::clear_pending() {
            try {
                assert((_pending_tx.size() == 0) ||
                       _pending_tx_session.valid());
                _pending_tx.clear();
                _pending_tx_session.reset();
            }
            FC_CAPTURE_AND_RETHROW()
        }

        void database::enable_plugins_on_push_transaction(bool value) {
            _enable_plugins_on_push_transaction = value;
        }

        void database::notify_pre_apply_operation(operation_notification &note) {
            note.trx_id = _current_trx_id;
            note.block = _current_block_num;
            note.trx_in_block = _current_trx_in_block;
            note.op_in_trx = _current_op_in_trx;

            if (!is_producing() || _enable_plugins_on_push_transaction) {
                CHAIN_TRY_NOTIFY(pre_apply_operation, note);
            }
        }

        void database::notify_post_apply_operation(const operation_notification &note) {
            if (!is_producing() || _enable_plugins_on_push_transaction) {
                CHAIN_TRY_NOTIFY(post_apply_operation, note);
            }
        }

        inline const void database::push_virtual_operation(const operation &op, bool force) {
            if (!force && _skip_virtual_ops ) {
                return;
            }

            FC_ASSERT(is_virtual_operation(op));
            operation_notification note(op);
            ++_current_virtual_op;
            note.virtual_op = _current_virtual_op;
            notify_pre_apply_operation(note);
            notify_post_apply_operation(note);
        }

        void database::notify_applied_block(const signed_block &block) {
            CHAIN_TRY_NOTIFY(applied_block, block)
        }

        void database::notify_on_pending_transaction(const signed_transaction &tx) {
            CHAIN_TRY_NOTIFY(on_pending_transaction, tx)
        }

        void database::notify_on_applied_transaction(const signed_transaction &tx) {
            CHAIN_TRY_NOTIFY(on_applied_transaction, tx)
        }

        account_name_type database::get_scheduled_witness(uint32_t slot_num) const {
            const dynamic_global_property_object &dpo = get_dynamic_global_properties();
            const witness_schedule_object &wso = get_witness_schedule_object();
            uint64_t current_aslot = dpo.current_aslot + slot_num;
            return wso.current_shuffled_witnesses[current_aslot %
                                                  wso.num_scheduled_witnesses];
        }

        fc::time_point_sec database::get_slot_time(uint32_t slot_num) const {
            if (slot_num == 0) {
                return fc::time_point_sec();
            }

            auto interval = CHAIN_BLOCK_INTERVAL;
            const dynamic_global_property_object &dpo = get_dynamic_global_properties();

            if (head_block_num() == 0) {
                // n.b. first block is at genesis_time plus one block interval
                fc::time_point_sec genesis_time = dpo.time - fc::seconds(CHAIN_BLOCK_INTERVAL);;
                return genesis_time + slot_num * interval;
            }

            int64_t head_block_abs_slot = head_block_time().sec_since_epoch() / interval;
            fc::time_point_sec head_slot_time(head_block_abs_slot * interval);

            // "slot 0" is head_slot_time
            // "slot 1" is head_slot_time,
            //   plus maint interval if head block is a maint block
            //   plus block interval if head block is not a maint block
            return head_slot_time + (slot_num * interval);
        }

        uint32_t database::get_slot_at_time(fc::time_point_sec when) const {
            fc::time_point_sec first_slot_time = get_slot_time(1);
            if (when < first_slot_time) {
                return 0;
            }
            return (when - first_slot_time).to_seconds() /
                   CHAIN_BLOCK_INTERVAL + 1;
        }

        void database::shares_sender_recalc_energy(const account_object &sender, asset tokens) {
            try {
                if(sender.effective_vesting_shares().amount.value>0){
                    asset shares = tokens;
                    modify(sender, [&](account_object &s) {
                        int64_t elapsed_seconds = (head_block_time() - s.last_vote_time).to_seconds();
                        int64_t regenerated_energy = (CHAIN_100_PERCENT * elapsed_seconds) / CHAIN_ENERGY_REGENERATION_SECONDS;
                        int64_t current_energy = std::min(int64_t(s.energy + regenerated_energy), int64_t(CHAIN_100_PERCENT));
                        int64_t new_energy = std::max(int64_t(current_energy - ((CHAIN_100_PERCENT * shares.amount.value) / s.effective_vesting_shares().amount.value)), int64_t(-CHAIN_100_PERCENT));
                        s.energy = int16_t(new_energy);
                        s.last_vote_time = head_block_time();
                    });
                }
            }
            FC_CAPTURE_AND_RETHROW((sender.name)(tokens))
        }
/**
 * @param to_account - the account to receive the new vesting shares
 * @param tokens - TOKEN to be converted to vesting shares
 */
        asset database::create_vesting(const account_object &to_account, asset tokens) {
            try {
                const auto &cprops = get_dynamic_global_properties();

                /**
                 *  The ratio of total_vesting_shares / total_vesting_fund should not
                 *  change as the result of the user adding funds
                 *
                 *  V / C = (V+Vn) / (C+Cn)
                 *
                 *  Simplifies to Vn = (V * Cn ) / C
                 *
                 *  If Cn equals o.amount, then we must solve for Vn to know how many new vesting shares
                 *  the user should receive.
                 *
                 *  128 bit math is requred due to multiplying of 64 bit numbers. This is done in asset and price.
                 */
                asset new_vesting = tokens * cprops.get_vesting_share_price();

                modify(to_account, [&](account_object &to) {
                    to.vesting_shares += new_vesting;
                });

                modify(cprops, [&](dynamic_global_property_object &props) {
                    props.total_vesting_fund += tokens;
                    props.total_vesting_shares += new_vesting;
                });

                adjust_proxied_witness_votes(to_account, new_vesting.amount);

                return new_vesting;
            }
            FC_CAPTURE_AND_RETHROW((to_account.name)(tokens))
        }

        void database::update_bandwidth_reserve_candidates() {
            if ((head_block_num() % CHAIN_BLOCKS_PER_HOUR ) != 0) return;
            uint32_t bandwidth_reserve_candidates = 1;
            const auto &gprops = get_dynamic_global_properties();
            const witness_schedule_object &consensus = get_witness_schedule_object();

            const auto &widx = get_index<account_index>().indices().get<by_id>();
            for (auto itr = widx.begin(); itr != widx.end(); ++itr) {
                if(itr->effective_vesting_shares().amount.value < consensus.median_props.bandwidth_reserve_below.amount.value){
                    if(time_point_sec(itr->last_bandwidth_update + CHAIN_BANDWIDTH_RESERVE_ACTIVE_TIME) >= head_block_time()){
                        ++bandwidth_reserve_candidates;
                    }
                }
            }
            modify(gprops, [&](dynamic_global_property_object &dgp) {
                dgp.bandwidth_reserve_candidates = bandwidth_reserve_candidates;
            });
        }

        void database::committee_processing() {
            const auto &props = get_dynamic_global_properties();
            const witness_schedule_object &consensus = get_witness_schedule_object();
            const auto &idx0 = get_index<committee_request_index>().indices().get<by_status>();
            auto itr0 = idx0.lower_bound(0);
            while (itr0 != idx0.end() &&
                   itr0->status == 0) {
                const auto &cur_request = *itr0;
                ++itr0;
                if(cur_request.end_time <= head_block_time()){
                    share_type max_rshares = 0;
                    share_type actual_rshares = 0;
                    share_type calculated_payment = 0;
                    share_type approve_min_shares = 0;
                    const auto &vote_idx = get_index<committee_vote_index>().indices().get<by_request_id>();
                    auto vote_itr = vote_idx.lower_bound(cur_request.request_id);
                    while (vote_itr != vote_idx.end() &&
                           vote_itr->request_id == cur_request.request_id) {
                        const auto &cur_vote = *vote_itr;
                        ++vote_itr;
                        const auto &voter_account = get_account(cur_vote.voter);
                        max_rshares+=voter_account.effective_vesting_shares().amount.value;
                        actual_rshares+=voter_account.effective_vesting_shares().amount.value*cur_vote.vote_percent/CHAIN_100_PERCENT;
                    }
                    approve_min_shares=props.total_vesting_shares.amount * consensus.median_props.committee_request_approve_min_percent / CHAIN_100_PERCENT;
                    if(approve_min_shares > max_rshares){
                        modify(cur_request, [&](committee_request_object &c) {
                            c.conclusion_time = head_block_time();
                            c.status = 2;
                        });
                        push_virtual_operation(committee_cancel_request_operation(cur_request.request_id));
                    }
                    else{
                        calculated_payment=( ( fc::uint128_t(cur_request.required_amount_max.amount) * ( fc::uint128_t(CHAIN_100_PERCENT) * fc::uint128_t(actual_rshares) / fc::uint128_t(max_rshares) ) ) / fc::uint128_t(CHAIN_100_PERCENT) ).to_uint64();
                        asset conclusion_payout_amount = asset(calculated_payment, TOKEN_SYMBOL);
                        if(cur_request.required_amount_min.amount > conclusion_payout_amount.amount){
                            modify(cur_request, [&](committee_request_object &c) {
                                c.conclusion_payout_amount=conclusion_payout_amount;
                                c.conclusion_time = head_block_time();
                                c.status = 3;
                            });
                            push_virtual_operation(committee_cancel_request_operation(cur_request.request_id));
                        }
                        else{
                            modify(cur_request, [&](committee_request_object &c) {
                                c.conclusion_payout_amount=conclusion_payout_amount;
                                c.conclusion_time = head_block_time();
                                c.remain_payout_amount=conclusion_payout_amount;
                                c.status = 4;
                            });
                            push_virtual_operation(committee_approve_request_operation(cur_request.request_id));
                        }
                    }
                }
            }
            if ((head_block_num() % COMMITTEE_REQUEST_PROCESSING ) != 0) return;
            uint32_t committee_payment_request_count = 1;
            const auto &idx4_count = get_index<committee_request_index>().indices().get<by_status>();
            auto itr4 = idx4_count.lower_bound(4);
            while (itr4 != idx4_count.end() &&
                   itr4->status == 4) {
                committee_payment_request_count++;
                ++itr4;
            }

            const auto &dgp = get_dynamic_global_properties();
            asset max_payment_per_request=asset(dgp.committee_fund.amount/committee_payment_request_count, TOKEN_SYMBOL);

            const auto &idx4 = get_index<committee_request_index>().indices().get<by_status>();
            auto itr = idx4.lower_bound(4);
            while (itr != idx4.end() &&
                   itr->status == 4) {
                const auto &cur_request = *itr;
                ++itr;
                share_type current_payment=std::min(max_payment_per_request.amount, cur_request.remain_payout_amount.amount).value;//int64
                const auto &worker_account = get_account(cur_request.worker);
                modify(worker_account, [&](account_object& w) {
                    w.balance += current_payment;
                });
                modify(dgp, [&](dynamic_global_property_object& dgpo) {
                    dgpo.committee_fund.amount -= current_payment;
                });
                push_virtual_operation(committee_pay_request_operation(cur_request.worker, cur_request.request_id, asset(current_payment, TOKEN_SYMBOL)));
                modify(cur_request, [&](committee_request_object& c) {
                    c.payout_amount.amount += current_payment;
                    c.remain_payout_amount.amount -= current_payment;
                    c.last_payout_time = head_block_time();
                    if(c.remain_payout_amount.amount.value<=0){
                        c.status = 5;
                        c.payout_time = head_block_time();
                        push_virtual_operation(committee_payout_request_operation(cur_request.request_id));
                    }
                });
            }
        }

        void database::update_witness_schedule() {
            if ((head_block_num() % ( CHAIN_MAX_WITNESSES * CHAIN_BLOCK_WITNESS_REPEAT ) ) != 0) return;
            vector<account_name_type> active_witnesses;
            active_witnesses.reserve(CHAIN_MAX_WITNESSES);

            vector<account_name_type> support_witnesses;
            support_witnesses.reserve(CHAIN_MAX_WITNESSES);

            /// Add the highest voted witnesses
            flat_set<witness_id_type> selected_voted;
            selected_voted.reserve(CHAIN_MAX_TOP_WITNESSES);

            const auto &widx = get_index<witness_index>().indices().get<by_vote_name>();
            for (auto itr = widx.begin();
                 itr != widx.end() &&
                 selected_voted.size() < CHAIN_MAX_TOP_WITNESSES;
                 ++itr) {
                if (itr->signing_key == public_key_type()) {
                    continue;
                }
                selected_voted.insert(itr->id);
                active_witnesses.push_back(itr->owner);
                modify(*itr, [&](witness_object &wo) { wo.schedule = witness_object::top; });
            }

            /// Add the running witnesses in the lead
            const witness_schedule_object &wso = get_witness_schedule_object();
            fc::uint128_t new_virtual_time = wso.current_virtual_time;
            const auto &schedule_idx = get_index<witness_index>().indices().get<by_schedule_time>();
            auto sitr = schedule_idx.begin();
            vector<decltype(sitr)> processed_witnesses;
            for (auto witness_count = selected_voted.size();
                 sitr != schedule_idx.end() &&
                 witness_count < CHAIN_MAX_WITNESSES;
                 ++sitr) {
                new_virtual_time = sitr->virtual_scheduled_time; /// everyone advances to at least this time
                processed_witnesses.push_back(sitr);

                if (sitr->signing_key == public_key_type()) {
                    continue;
                } /// skip witnesses without a valid block signing key

                if (selected_voted.find(sitr->id) == selected_voted.end()) {
                    support_witnesses.push_back(sitr->owner);
                    modify(*sitr, [&](witness_object &wo) { wo.schedule = witness_object::support; });
                    ++witness_count;
                }
            }

            /// Update virtual schedule of processed witnesses
            bool reset_virtual_time = false;
            for (auto itr = processed_witnesses.begin();
                 itr != processed_witnesses.end(); ++itr) {
                auto new_virtual_scheduled_time = new_virtual_time +
                                                  VIRTUAL_SCHEDULE_LAP_LENGTH2 /
                                                  ((*itr)->votes.value + 1);
                if (new_virtual_scheduled_time < new_virtual_time) {
                    reset_virtual_time = true; /// overflow
                    break;
                }
                modify(*(*itr), [&](witness_object &wo) {
                    wo.virtual_position = fc::uint128_t();
                    wo.virtual_last_update = new_virtual_time;
                    wo.virtual_scheduled_time = new_virtual_scheduled_time;
                });
            }
            if (reset_virtual_time) {
                new_virtual_time = fc::uint128_t();
                reset_virtual_schedule_time();
            }

            FC_ASSERT( ( active_witnesses.size() + support_witnesses.size() ) <= CHAIN_MAX_WITNESSES, "Number of active witnesses does cannot be more CHAIN_MAX_WITNESSES",
                    ("active_witnesses.size()", active_witnesses.size())("support_witnesses.size()", support_witnesses.size())("CHAIN_MAX_WITNESSES", CHAIN_MAX_WITNESSES));

            auto majority_version = wso.majority_version;

            flat_map<version, uint32_t, std::greater<version>> witness_versions;
            flat_map<std::tuple<hardfork_version, time_point_sec>, uint32_t> hardfork_version_votes;

            for (uint32_t i = 0; i < wso.num_scheduled_witnesses; i+=CHAIN_BLOCK_WITNESS_REPEAT) {
                auto witness = get_witness(wso.current_shuffled_witnesses[i]);
                if (witness_versions.find(witness.running_version) ==
                    witness_versions.end()) {
                    witness_versions[witness.running_version] = 1;
                } else {
                    witness_versions[witness.running_version] += 1;
                }

                auto version_vote = std::make_tuple(witness.hardfork_version_vote, witness.hardfork_time_vote);
                if (hardfork_version_votes.find(version_vote) ==
                    hardfork_version_votes.end()) {
                    hardfork_version_votes[version_vote] = 1;
                } else {
                    hardfork_version_votes[version_vote] += 1;
                }
            }

            int witnesses_on_version = 0;
            auto ver_itr = witness_versions.begin();

            // The map should be sorted highest version to smallest, so we iterate until we hit the majority of witnesses on at least this version
            while (ver_itr != witness_versions.end()) {
                witnesses_on_version += ver_itr->second;

                if (witnesses_on_version >=
                    CHAIN_HARDFORK_REQUIRED_WITNESSES) {
                    majority_version = ver_itr->first;
                    break;
                }

                ++ver_itr;
            }

            auto hf_itr = hardfork_version_votes.begin();

            while (hf_itr != hardfork_version_votes.end()) {
                if (hf_itr->second >= CHAIN_HARDFORK_REQUIRED_WITNESSES) {
                    const auto &hfp = get_hardfork_property_object();
                    if (hfp.next_hardfork != std::get<0>(hf_itr->first) ||
                        hfp.next_hardfork_time !=
                        std::get<1>(hf_itr->first)) {

                        modify(hfp, [&](hardfork_property_object &hpo) {
                            hpo.next_hardfork = std::get<0>(hf_itr->first);
                            hpo.next_hardfork_time = std::get<1>(hf_itr->first);
                        });
                    }
                    break;
                }

                ++hf_itr;
            }

            // We no longer have a majority
            if (hf_itr == hardfork_version_votes.end()) {
                modify(get_hardfork_property_object(), [&](hardfork_property_object &hpo) {
                    hpo.next_hardfork = hpo.current_hardfork_version;
                });
            }

            modify(wso, [&](witness_schedule_object &_wso) {
                // active witnesses has exactly CHAIN_MAX_WITNESSES elements, asserted above
                size_t j = 0;
                size_t support_witnesses_count = support_witnesses.size();
                size_t active_witnesses_count = active_witnesses.size();
                size_t sum_witnesses_count = support_witnesses_count+active_witnesses_count;
                for( size_t i = 0; i < sum_witnesses_count; i++ )
                {
                    if(active_witnesses_count > 0){
                        --active_witnesses_count;
                        for(int repeat=0; repeat < CHAIN_BLOCK_WITNESS_REPEAT; ++repeat){
                            _wso.current_shuffled_witnesses[j] = active_witnesses[i];
                            ++j;
                        }
                    }
                    if(support_witnesses_count > 0){
                        --support_witnesses_count;
                        for(int repeat=0; repeat < CHAIN_BLOCK_WITNESS_REPEAT; ++repeat){
                            _wso.current_shuffled_witnesses[j] = support_witnesses[i];
                            ++j;
                        }
                    }
                }

                for (size_t i = sum_witnesses_count * CHAIN_BLOCK_WITNESS_REPEAT;
                     i < ( CHAIN_MAX_WITNESSES * CHAIN_BLOCK_WITNESS_REPEAT ); i++) {
                    _wso.current_shuffled_witnesses[i] = account_name_type();
                }

                _wso.num_scheduled_witnesses = std::max<uint8_t>(sum_witnesses_count * CHAIN_BLOCK_WITNESS_REPEAT , 1);

                /* // VIZ remove randomization
                /// shuffle current shuffled witnesses
                auto now_hi =
                        uint64_t(head_block_time().sec_since_epoch()) << 32;
                for (uint32_t i = 0; i < _wso.num_scheduled_witnesses; ++i) {
                    /// High performance random generator
                    /// http://xorshift.di.unimi.it/
                    uint64_t k = now_hi + uint64_t(i) * 2685821657736338717ULL;
                    k ^= (k >> 12);
                    k ^= (k << 25);
                    k ^= (k >> 27);
                    k *= 2685821657736338717ULL;

                    uint32_t jmax = _wso.num_scheduled_witnesses - i;
                    uint32_t j = i + k % jmax;
                    std::swap(_wso.current_shuffled_witnesses[i],
                            _wso.current_shuffled_witnesses[j]);
                }
                */

                _wso.current_virtual_time = new_virtual_time;
                _wso.next_shuffle_block_num =
                        head_block_num() + _wso.num_scheduled_witnesses;
                _wso.majority_version = majority_version;
            });

            update_median_witness_props();
        }

        void database::update_median_witness_props() {
            const witness_schedule_object &wso = get_witness_schedule_object();

            /// fetch all witness objects
            vector<const witness_object *> active;
            active.reserve(wso.num_scheduled_witnesses);
            for (int i = 0; i < wso.num_scheduled_witnesses; i+=CHAIN_BLOCK_WITNESS_REPEAT) {
                active.push_back(&get_witness(wso.current_shuffled_witnesses[i]));
            }

            chain_properties median_props;

            auto calc_median = [&](auto&& param) {
                std::nth_element(
                    active.begin(), active.begin() + active.size() / 2, active.end(),
                    [&](const auto* a, const auto* b) {
                        return a->props.*param < b->props.*param;
                    }
                );
                median_props.*param = active[active.size() / 2]->props.*param;
            };

            calc_median(&chain_properties::account_creation_fee);
            calc_median(&chain_properties::maximum_block_size);
            calc_median(&chain_properties::create_account_delegation_ratio);
            calc_median(&chain_properties::create_account_delegation_time);
            calc_median(&chain_properties::min_delegation);
            calc_median(&chain_properties::min_curation_percent);
            calc_median(&chain_properties::max_curation_percent);
            calc_median(&chain_properties::bandwidth_reserve_percent);
            calc_median(&chain_properties::bandwidth_reserve_below);
            calc_median(&chain_properties::flag_energy_additional_cost);
            calc_median(&chain_properties::vote_accounting_min_rshares);
            calc_median(&chain_properties::committee_request_approve_min_percent);

            modify(wso, [&](witness_schedule_object &_wso) {
                _wso.median_props = median_props;
            });

            modify(get_dynamic_global_properties(), [&](dynamic_global_property_object &_dgpo) {
                _dgpo.maximum_block_size = median_props.maximum_block_size;
            });
        }

        void database::adjust_proxied_witness_votes(const account_object &a,
                const std::array<share_type,
                        CHAIN_MAX_PROXY_RECURSION_DEPTH + 1> &delta,
                int depth) {
            if (a.proxy != CHAIN_PROXY_TO_SELF_ACCOUNT) {
                /// nested proxies are not supported, vote will not propagate
                if (depth >= CHAIN_MAX_PROXY_RECURSION_DEPTH) {
                    return;
                }

                const auto &proxy = get_account(a.proxy);

                modify(proxy, [&](account_object &a) {
                    for (int i = CHAIN_MAX_PROXY_RECURSION_DEPTH - depth - 1;
                         i >= 0; --i) {
                        a.proxied_vsf_votes[i + depth] += delta[i];
                    }
                });

                adjust_proxied_witness_votes(proxy, delta, depth + 1);
            } else {
                share_type total_delta = 0;
                for (int i = CHAIN_MAX_PROXY_RECURSION_DEPTH - depth;
                     i >= 0; --i) {
                    total_delta += delta[i];
                }
                adjust_witness_votes(a, total_delta);
            }
        }

        void database::adjust_proxied_witness_votes(const account_object &a, share_type delta, int depth) {
            if (a.proxy != CHAIN_PROXY_TO_SELF_ACCOUNT) {
                /// nested proxies are not supported, vote will not propagate
                if (depth >= CHAIN_MAX_PROXY_RECURSION_DEPTH) {
                    return;
                }

                const auto &proxy = get_account(a.proxy);

                modify(proxy, [&](account_object &a) {
                    a.proxied_vsf_votes[depth] += delta;
                });

                adjust_proxied_witness_votes(proxy, delta, depth + 1);
            } else {
                adjust_witness_votes(a, delta);
            }
        }

        void database::adjust_witness_votes(const account_object &a, share_type delta) {
            const auto &vidx = get_index<witness_vote_index>().indices().get<by_account_witness>();
            auto itr = vidx.lower_bound(boost::make_tuple(a.id, witness_id_type()));
            while (itr != vidx.end() && itr->account == a.id) {
                adjust_witness_vote(get(itr->witness), delta);
                ++itr;
            }
        }

        void database::adjust_witness_vote(const witness_object &witness, share_type delta) {
            const witness_schedule_object &wso = get_witness_schedule_object();
            modify(witness, [&](witness_object &w) {
                auto delta_pos = w.votes.value * (wso.current_virtual_time -
                                                  w.virtual_last_update);
                w.virtual_position += delta_pos;

                w.virtual_last_update = wso.current_virtual_time;
                w.votes += delta;
                FC_ASSERT(w.votes <=
                          get_dynamic_global_properties().total_vesting_shares.amount, "", ("w.votes", w.votes)("props", get_dynamic_global_properties().total_vesting_shares));

                w.virtual_scheduled_time = w.virtual_last_update +
                                           (VIRTUAL_SCHEDULE_LAP_LENGTH2 -
                                            w.virtual_position) /
                                           (w.votes.value + 1);

                /** witnesses with a low number of votes could overflow the time field and end up with a scheduled time in the past */
                if (w.virtual_scheduled_time < wso.current_virtual_time) {
                    w.virtual_scheduled_time = fc::uint128_t::max_value();
                }
            });
        }

        void database::clear_witness_votes(const account_object &a) {
            const auto &vidx = get_index<witness_vote_index>().indices().get<by_account_witness>();
            auto itr = vidx.lower_bound(boost::make_tuple(a.id, witness_id_type()));
            while (itr != vidx.end() && itr->account == a.id) {
                const auto &current = *itr;
                ++itr;
                remove(current);
            }

            modify(a, [&](account_object &acc) {
                acc.witnesses_voted_for = 0;
            });
        }

        void database::clear_null_account_balance() {
            const auto &null_account = get_account(CHAIN_NULL_ACCOUNT);
            asset total_tokens(0, TOKEN_SYMBOL);

            if (null_account.balance.amount > 0) {
                total_tokens += null_account.balance;
                adjust_balance(null_account, -null_account.balance);
            }

            if (null_account.vesting_shares.amount > 0) {
                const auto &gpo = get_dynamic_global_properties();
                auto converted_tokens = null_account.vesting_shares *
                                       gpo.get_vesting_share_price();

            modify(gpo, [&](dynamic_global_property_object &g) {
                g.total_vesting_shares -= null_account.vesting_shares;
                g.total_vesting_fund -= converted_tokens;
            });

            modify(null_account, [&](account_object &a) {
                a.vesting_shares.amount = 0;
            });

                total_tokens += converted_tokens;
            }

            if (total_tokens.amount > 0) {
                burn_asset(-total_tokens);
            }
        }

        void database::clear_anonymous_account_balance() {
            const auto &anonymous_account = get_account(CHAIN_ANONYMOUS_ACCOUNT);
            asset total_tokens(0, TOKEN_SYMBOL);

            if (anonymous_account.balance.amount > 0) {
                total_tokens += anonymous_account.balance;
                adjust_balance(anonymous_account, -anonymous_account.balance);
            }

            if (anonymous_account.vesting_shares.amount > 0) {
                const auto &gpo = get_dynamic_global_properties();
                auto converted_tokens = anonymous_account.vesting_shares *
                                       gpo.get_vesting_share_price();

            modify(gpo, [&](dynamic_global_property_object &g) {
                g.total_vesting_shares -= anonymous_account.vesting_shares;
                g.total_vesting_fund -= converted_tokens;
            });

            modify(anonymous_account, [&](account_object &a) {
                a.vesting_shares.amount = 0;
            });

                total_tokens += converted_tokens;
            }

            if (total_tokens.amount > 0) {
                burn_asset(-total_tokens);
            }
        }

        void database::claim_committee_account_balance() {
            const auto &gpo = get_dynamic_global_properties();
            const auto &committee_account = get_account(CHAIN_COMMITTEE_ACCOUNT);
            asset total_tokens(0, TOKEN_SYMBOL);

            if (committee_account.balance.amount > 0) {
                total_tokens += committee_account.balance;
                adjust_balance(committee_account, -committee_account.balance);
            }

            if (committee_account.vesting_shares.amount > 0) {
                auto converted_tokens = committee_account.vesting_shares *
                                       gpo.get_vesting_share_price();

                modify(gpo, [&](dynamic_global_property_object &g) {
                    g.total_vesting_shares -= committee_account.vesting_shares;
                    g.total_vesting_fund -= converted_tokens;
                });

                modify(committee_account, [&](account_object &a) {
                    a.vesting_shares.amount = 0;
                });

                total_tokens += converted_tokens;
            }

            if (total_tokens.amount > 0) {
                burn_asset(-total_tokens);
                modify(gpo, [&](dynamic_global_property_object &g) {
                    g.committee_fund += total_tokens;
                });
            }
        }

/**
 * This method recursively tallies children_rshares for this post plus all of its parents,
 * TODO: this method can be skipped for validation-only nodes
 */
        void database::adjust_rshares(const content_object &c, fc::uint128_t old_rshares, fc::uint128_t new_rshares) {
            modify(c, [&](content_object &content) {
                content.children_rshares -= old_rshares;
                content.children_rshares += new_rshares;
            });
            if (c.depth) {
                adjust_rshares(get_content(c.parent_author, c.parent_permlink), old_rshares, new_rshares);
            } else {
                const auto &cprops = get_dynamic_global_properties();
                modify(cprops, [&](dynamic_global_property_object &p) {
                    p.total_reward_shares -= old_rshares;
                    p.total_reward_shares += new_rshares;
                });
            }
        }

        void database::update_owner_authority(const account_object &account, const authority &owner_authority) {
            if (head_block_num() >= 1) {
                create<owner_authority_history_object>([&](owner_authority_history_object &hist) {
                    hist.account = account.name;
                    hist.previous_owner_authority = get<account_authority_object, by_account>(account.name).owner;
                    hist.last_valid_time = head_block_time();
                });
            }

            modify(get<account_authority_object, by_account>(account.name), [&](account_authority_object &auth) {
                auth.owner = owner_authority;
                auth.last_owner_update = head_block_time();
            });
        }

        void database::process_vesting_withdrawals() {
            const auto &widx = get_index<account_index>().indices().get<by_next_vesting_withdrawal>();
            const auto &didx = get_index<withdraw_vesting_route_index>().indices().get<by_withdraw_route>();
            auto current = widx.begin();

            const auto &cprops = get_dynamic_global_properties();

            while (current != widx.end() &&
                   current->next_vesting_withdrawal <= head_block_time()) {
                const auto &from_account = *current;
                ++current;

                /**
      *  Let T = total tokens in vesting fund
      *  Let V = total vesting shares
      *  Let v = total vesting shares being cashed out
      *
      *  The user may withdraw vT / V tokens
      */
                share_type to_withdraw;
                if (from_account.to_withdraw - from_account.withdrawn <
                    from_account.vesting_withdraw_rate.amount) {
                    to_withdraw = std::min(from_account.vesting_shares.amount,
                            from_account.to_withdraw %
                            from_account.vesting_withdraw_rate.amount).value;
                } else {
                    to_withdraw = std::min(from_account.vesting_shares.amount, from_account.vesting_withdraw_rate.amount).value;
                }

                if(to_withdraw<0){
                    to_withdraw=0;
                }

                share_type vests_deposited_as_tokens = 0;
                share_type vests_deposited_as_vests = 0;
                asset total_tokens_converted = asset(0, TOKEN_SYMBOL);

                // Do two passes, the first for shares, the second for tokens. Try to maintain as much accuracy for shares as possible.
                for (auto itr = didx.upper_bound(boost::make_tuple(from_account.id, account_id_type()));
                     itr != didx.end() && itr->from_account == from_account.id;
                     ++itr) {
                    if (itr->auto_vest) {
                        share_type to_deposit = (
                                (fc::uint128_t(to_withdraw.value) *
                                 itr->percent) /
                                CHAIN_100_PERCENT).to_uint64();
                        vests_deposited_as_vests += to_deposit;

                        if (to_deposit > 0) {
                            const auto &to_account = get(itr->to_account);

                            modify(to_account, [&](account_object &a) {
                                a.vesting_shares.amount += to_deposit;
                            });

                            adjust_proxied_witness_votes(to_account, to_deposit);

                            push_virtual_operation(fill_vesting_withdraw_operation(from_account.name, to_account.name, asset(to_deposit, SHARES_SYMBOL), asset(to_deposit, SHARES_SYMBOL)));
                        }
                    }
                }

                for (auto itr = didx.upper_bound(boost::make_tuple(from_account.id, account_id_type()));
                     itr != didx.end() && itr->from_account == from_account.id;
                     ++itr) {
                    if (!itr->auto_vest) {
                        const auto &to_account = get(itr->to_account);

                        share_type to_deposit = (
                                (fc::uint128_t(to_withdraw.value) *
                                 itr->percent) /
                                CHAIN_100_PERCENT).to_uint64();
                        vests_deposited_as_tokens += to_deposit;
                        auto converted_tokens = asset(to_deposit, SHARES_SYMBOL) *
                                               cprops.get_vesting_share_price();
                        total_tokens_converted += converted_tokens;

                        if (to_deposit > 0) {
                            modify(to_account, [&](account_object &a) {
                                a.balance += converted_tokens;
                            });

                            modify(cprops, [&](dynamic_global_property_object &o) {
                                o.total_vesting_fund -= converted_tokens;
                                o.total_vesting_shares.amount -= to_deposit;
                            });

                            push_virtual_operation(fill_vesting_withdraw_operation(from_account.name, to_account.name, asset(to_deposit, SHARES_SYMBOL), converted_tokens));
                        }
                    }
                }

                share_type to_convert = to_withdraw - vests_deposited_as_tokens -
                                        vests_deposited_as_vests;
                if(to_convert<0){
                    to_convert=0;
                }

                auto converted_tokens = asset(to_convert, SHARES_SYMBOL) *
                                       cprops.get_vesting_share_price();

                shares_sender_recalc_energy(from_account,asset(vests_deposited_as_tokens, SHARES_SYMBOL)+asset(to_convert, SHARES_SYMBOL));

                modify(from_account, [&](account_object &a) {
                    a.vesting_shares.amount -= to_withdraw;
                    a.balance += converted_tokens;
                    a.withdrawn += to_withdraw;

                    if (a.withdrawn >= a.to_withdraw ||
                        a.vesting_shares.amount == 0) {
                        a.vesting_withdraw_rate.amount = 0;
                        a.next_vesting_withdrawal = fc::time_point_sec::maximum();
                    } else {
                        a.next_vesting_withdrawal += fc::seconds(CHAIN_VESTING_WITHDRAW_INTERVAL_SECONDS);
                    }
                });

                modify(cprops, [&](dynamic_global_property_object &o) {
                    o.total_vesting_fund -= converted_tokens;
                    o.total_vesting_shares.amount -= to_convert;
                });

                if (to_withdraw > 0) {
                    adjust_proxied_witness_votes(from_account, -to_withdraw);
                    push_virtual_operation(fill_vesting_withdraw_operation(from_account.name, from_account.name, asset(to_withdraw, SHARES_SYMBOL), converted_tokens));
                }
            }
        }

        void database::adjust_total_payout(
                const content_object &cur,
                const asset &payout,
                const asset &shares_payout,
                const asset &curator_value,
                const asset &beneficiary_value
        ) {
            modify(cur, [&](content_object &c) {
                if (c.payout_value.symbol == payout.symbol) {
                    c.payout_value += payout;
                    c.shares_payout_value += shares_payout;
                    c.curator_payout_value += curator_value;
                    c.beneficiary_payout_value += beneficiary_value;
                }
            });
        }

        void database::cashout_content_helper(const content_object &content) {
            try {
                if (content.net_rshares > 0) {
                    uint128_t reward_tokens = uint128_t(
                         claim_rshare_reward(content.net_rshares));

                    asset total_payout;
                    if (reward_tokens > 0) {
                        const witness_schedule_object &props = get_witness_schedule_object();
                        auto consensus_curation_percent=std::min(content.curation_percent,props.median_props.max_curation_percent);
                        consensus_curation_percent=std::max(consensus_curation_percent,props.median_props.min_curation_percent);
                        share_type curation_tokens = ((reward_tokens *
                                                      consensus_curation_percent) /
                                                      CHAIN_100_PERCENT).to_uint64();

                        share_type author_tokens = reward_tokens.to_uint64() - curation_tokens;

                        // VIZ: pay_curators
                        asset total_curation_shares = asset(0, SHARES_SYMBOL);
                        uint128_t total_weight(content.total_vote_weight);
                        share_type unclaimed_rewards = curation_tokens;
                        if (content.total_vote_weight > 0) {
                            const auto &cvidx = get_index<content_vote_index>().indices().get<by_content_weight_voter>();
                            auto itr = cvidx.lower_bound(content.id);
                            while (itr != cvidx.end() && itr->content == content.id) {
                                uint128_t weight(itr->weight);
                                auto claim = ((curation_tokens.value * weight) /
                                              total_weight).to_uint64();
                                if (claim > 0) // min_amt is non-zero satoshis
                                {
                                    unclaimed_rewards -= claim;
                                    const auto &voter = get(itr->voter);
                                    auto reward = create_vesting(voter, asset(claim, TOKEN_SYMBOL));
                                    total_curation_shares += asset( reward.amount, SHARES_SYMBOL );
                                    push_virtual_operation(curation_reward_operation(voter.name, reward, content.author, to_string(content.permlink)));
#ifndef IS_LOW_MEM
                                    modify(voter, [&](account_object &a) {
                                        a.curation_rewards += claim;
                                    });
#endif
                                }
                                ++itr;
                            }
                        }

                        author_tokens += unclaimed_rewards;

                        share_type total_beneficiary = 0;
                        asset total_beneficiary_shares = asset(0, SHARES_SYMBOL);

                        for (auto &b : content.beneficiaries) {
                            auto benefactor_tokens = (author_tokens * b.weight) / CHAIN_100_PERCENT;
                            auto shares_created = create_vesting(get_account(b.account), benefactor_tokens);
                            total_beneficiary_shares += asset( shares_created.amount, SHARES_SYMBOL );
                            push_virtual_operation(
                                content_benefactor_reward_operation(
                                    b.account, content.author, to_string(content.permlink), shares_created));
                            total_beneficiary += benefactor_tokens;
                        }

                        author_tokens -= total_beneficiary;

                        auto payout_value = author_tokens / 2;
                        auto shares_payout_value = author_tokens - payout_value;

                        const auto &author = get_account(content.author);
                        auto shares_created = create_vesting(author, shares_payout_value);
                        adjust_balance(author, payout_value);

                        adjust_total_payout(
                                content,
                                asset(payout_value, TOKEN_SYMBOL),
                                shares_created,
                                total_curation_shares,
                                total_beneficiary_shares
                        );

                        // stats only.. TODO: Move to plugin...
                        total_payout = asset(reward_tokens.to_uint64(), TOKEN_SYMBOL);

                        push_virtual_operation(author_reward_operation(content.author, to_string(content.permlink), payout_value, shares_created));
                        push_virtual_operation(content_reward_operation(content.author, to_string(content.permlink), total_payout));

#ifndef IS_LOW_MEM
                        modify(content, [&](content_object &c) {
                            c.author_rewards += author_tokens;
                            c.consensus_curation_percent = consensus_curation_percent;
                        });

                        modify(get_account(content.author), [&](account_object &a) {
                            a.posting_rewards += author_tokens;
                        });
#endif

                    }

                    fc::uint128_t old_rshares = content.net_rshares.value;
                    adjust_rshares(content, old_rshares, 0);
                }

                modify(content, [&](content_object &c) {
                    /**
                    * A payout is only made for positive rshares, negative rshares hang around
                    * for the next time this post might get an upvote.
                    */
                    if (c.net_rshares > 0) {
                        c.net_rshares = 0;
                    }
                    c.abs_rshares = 0;
                    c.vote_rshares = 0;
                    c.total_vote_weight = 0;
                    c.cashout_time = fc::time_point_sec::maximum();
                    c.last_payout = head_block_time();
                });

                push_virtual_operation(content_payout_update_operation(content.author, to_string(content.permlink)));

                const auto &vote_idx = get_index<content_vote_index>().indices().get<by_content_voter>();
                auto vote_itr = vote_idx.lower_bound(content.id);
                while (vote_itr != vote_idx.end() &&
                       vote_itr->content == content.id) {
                    const auto &cur_vote = *vote_itr;
                    ++vote_itr;
                    if (calculate_discussion_payout_time(content) != fc::time_point_sec::maximum()) {
                        modify(cur_vote, [&](content_vote_object &cvo) {
                            cvo.num_changes = -1;
                        });
                    }
                }
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::process_content_cashout() {
            const auto &cidx = get_index<content_index>().indices().get<by_cashout_time>();
            const auto block_time = head_block_time();

            auto current = cidx.begin();
            while (current != cidx.end() && current->cashout_time <= block_time) {
                cashout_content_helper(*current);
                current = cidx.begin();
            }
        }

        void database::process_funds() {
            const auto &props = get_dynamic_global_properties();
            share_type inflation_rate = int64_t( CHAIN_FIXED_INFLATION );
            share_type new_supply = int64_t( CHAIN_INIT_SUPPLY );
            share_type inflation_per_year = inflation_rate * int64_t( CHAIN_INIT_SUPPLY ) / int64_t( CHAIN_100_PERCENT );
            new_supply += inflation_per_year;
            int circles = props.head_block_number / CHAIN_BLOCKS_PER_YEAR;
            if(circles > 0)
            {
               for( int itr = 0; itr < circles; ++itr )
               {
                  inflation_per_year = ( new_supply * inflation_rate ) / int64_t( CHAIN_100_PERCENT );
                  new_supply += inflation_per_year;
               }
            }
            share_type inflation_per_block = inflation_per_year / int64_t( CHAIN_BLOCKS_PER_YEAR );

            /*ilog( "Inflation status: props.head_block_number=${h1}, inflation_per_year=${h2}, new_supply=${h3}, inflation_per_block=${h4}",
               ("h1",props.head_block_number)("h2", inflation_per_year)("h3",new_supply)("h4",inflation_per_block)
            );*/
            auto content_reward = ( inflation_per_block * CHAIN_REWARD_FUND_PERCENT ) / CHAIN_100_PERCENT;
            auto vesting_reward = ( inflation_per_block * CHAIN_VESTING_FUND_PERCENT ) / CHAIN_100_PERCENT; /// 15% to vesting fund
            auto committee_reward = ( inflation_per_block * CHAIN_COMMITTEE_FUND_PERCENT ) / CHAIN_100_PERCENT;
            auto witness_reward = inflation_per_block - content_reward - vesting_reward - committee_reward; /// Remaining 10% to witness pay

            const auto& cwit = get_witness( props.current_witness );

            inflation_per_block = content_reward + vesting_reward + committee_reward + witness_reward;
            /*
            elog( "Final inflation_per_block=${h1}, content_reward=${h2}, committee_reward=${h3}, witness_reward=${h4}, vesting_reward=${h5}",
               ("h1",inflation_per_block)("h2", content_reward)("h3",committee_reward)("h4",witness_reward)("h5",vesting_reward)
            );
            */
            modify( props, [&]( dynamic_global_property_object& p )
            {
               p.total_vesting_fund += asset( vesting_reward, TOKEN_SYMBOL );
               p.committee_fund += asset( committee_reward, TOKEN_SYMBOL );
               p.total_reward_fund += asset( content_reward, TOKEN_SYMBOL );
               p.current_supply += asset( inflation_per_block, TOKEN_SYMBOL );
            });

            auto witness_reward_shares = create_vesting(get_account(cwit.owner), asset(witness_reward, TOKEN_SYMBOL));
            push_virtual_operation(witness_reward_operation(cwit.owner,witness_reward_shares));
        }

/**
 *  This method reduces the rshare^2 supply and returns the number of tokens are
 *  redeemed.
 */
        share_type database::claim_rshare_reward(share_type rshares) {
        try {
                FC_ASSERT(rshares > 0);

                const auto &props = get_dynamic_global_properties();

                u256 rs(rshares.value);
                u256 rf(props.total_reward_fund.amount.value);
                u256 total_rshares = to256(props.total_reward_shares);

                u256 rs2 = to256(rshares.value);

                u256 payout_u256 = (rf * rs2) / total_rshares;
                FC_ASSERT(payout_u256 <=
                          u256(uint64_t(std::numeric_limits<int64_t>::max())));
                uint64_t payout = static_cast< uint64_t >( payout_u256 );

                modify(props, [&](dynamic_global_property_object &p) {
                    p.total_reward_fund.amount -= payout;
                });

                return payout;
            } FC_CAPTURE_AND_RETHROW((rshares))
        }

        void database::account_recovery_processing() {
            // Clear expired recovery requests
            const auto &rec_req_idx = get_index<account_recovery_request_index>().indices().get<by_expiration>();
            auto rec_req = rec_req_idx.begin();

            while (rec_req != rec_req_idx.end() &&
                   rec_req->expires <= head_block_time()) {
                remove(*rec_req);
                rec_req = rec_req_idx.begin();
            }

            // Clear invalid historical authorities
            const auto &hist_idx = get_index<owner_authority_history_index>().indices(); //by id
            auto hist = hist_idx.begin();

            while (hist != hist_idx.end() && time_point_sec(
                    hist->last_valid_time +
                    CHAIN_OWNER_AUTH_RECOVERY_PERIOD) < head_block_time()) {
                remove(*hist);
                hist = hist_idx.begin();
            }

            // Apply effective recovery_account changes
            const auto &change_req_idx = get_index<change_recovery_account_request_index>().indices().get<by_effective_date>();
            auto change_req = change_req_idx.begin();

            while (change_req != change_req_idx.end() &&
                   change_req->effective_on <= head_block_time()) {
                modify(get_account(change_req->account_to_recover), [&](account_object &a) {
                    a.recovery_account = change_req->recovery_account;
                });

                remove(*change_req);
                change_req = change_req_idx.begin();
            }
        }

        void database::expire_escrow_ratification() {
            const auto &escrow_idx = get_index<escrow_index>().indices().get<by_ratification_deadline>();
            auto escrow_itr = escrow_idx.lower_bound(false);

            while (escrow_itr != escrow_idx.end() &&
                   !escrow_itr->is_approved() &&
                   escrow_itr->ratification_deadline <= head_block_time()) {
                const auto &old_escrow = *escrow_itr;
                ++escrow_itr;

                const auto &from_account = get_account(old_escrow.from);
                adjust_balance(from_account, old_escrow.token_balance);
                adjust_balance(from_account, old_escrow.pending_fee);

                remove(old_escrow);
            }
        }

        time_point_sec database::head_block_time() const {
            return get_dynamic_global_properties().time;
        }

        uint32_t database::head_block_num() const {
            return get_dynamic_global_properties().head_block_number;
        }

        block_id_type database::head_block_id() const {
            return get_dynamic_global_properties().head_block_id;
        }

        uint32_t database::last_non_undoable_block_num() const {
            return get_dynamic_global_properties().last_irreversible_block_num;
        }

        void database::initialize_evaluators() {
            _my->_evaluator_registry.register_evaluator<vote_evaluator>();
            _my->_evaluator_registry.register_evaluator<content_evaluator>();
            _my->_evaluator_registry.register_evaluator<delete_content_evaluator>();
            _my->_evaluator_registry.register_evaluator<transfer_evaluator>();
            _my->_evaluator_registry.register_evaluator<transfer_to_vesting_evaluator>();
            _my->_evaluator_registry.register_evaluator<withdraw_vesting_evaluator>();
            _my->_evaluator_registry.register_evaluator<set_withdraw_vesting_route_evaluator>();
            _my->_evaluator_registry.register_evaluator<account_create_evaluator>();
            _my->_evaluator_registry.register_evaluator<account_update_evaluator>();
            _my->_evaluator_registry.register_evaluator<account_metadata_evaluator>();
            _my->_evaluator_registry.register_evaluator<witness_update_evaluator>();
            _my->_evaluator_registry.register_evaluator<account_witness_vote_evaluator>();
            _my->_evaluator_registry.register_evaluator<account_witness_proxy_evaluator>();
            _my->_evaluator_registry.register_evaluator<custom_evaluator>();
            _my->_evaluator_registry.register_evaluator<request_account_recovery_evaluator>();
            _my->_evaluator_registry.register_evaluator<recover_account_evaluator>();
            _my->_evaluator_registry.register_evaluator<change_recovery_account_evaluator>();
            _my->_evaluator_registry.register_evaluator<escrow_transfer_evaluator>();
            _my->_evaluator_registry.register_evaluator<escrow_approve_evaluator>();
            _my->_evaluator_registry.register_evaluator<escrow_dispute_evaluator>();
            _my->_evaluator_registry.register_evaluator<escrow_release_evaluator>();
            _my->_evaluator_registry.register_evaluator<delegate_vesting_shares_evaluator>();
            _my->_evaluator_registry.register_evaluator<proposal_create_evaluator>();
            _my->_evaluator_registry.register_evaluator<proposal_update_evaluator>();
            _my->_evaluator_registry.register_evaluator<proposal_delete_evaluator>();
            _my->_evaluator_registry.register_evaluator<chain_properties_update_evaluator>();
            _my->_evaluator_registry.register_evaluator<committee_worker_create_request_evaluator>();
            _my->_evaluator_registry.register_evaluator<committee_worker_cancel_request_evaluator>();
            _my->_evaluator_registry.register_evaluator<committee_vote_request_evaluator>();
            _my->_evaluator_registry.register_evaluator<create_invite_evaluator>();
            _my->_evaluator_registry.register_evaluator<claim_invite_balance_evaluator>();
            _my->_evaluator_registry.register_evaluator<invite_registration_evaluator>();
        }

        void database::set_custom_operation_interpreter(const std::string &id, std::shared_ptr<custom_operation_interpreter> registry) {
            bool inserted = _custom_operation_interpreters.emplace(id, registry).second;
            // This assert triggering means we're mis-configured (multiple registrations of custom JSON evaluator for same ID)
            FC_ASSERT(inserted);
        }

        std::shared_ptr<custom_operation_interpreter> database::get_custom_evaluator(const std::string &id) {
            auto it = _custom_operation_interpreters.find(id);
            if (it != _custom_operation_interpreters.end()) {
                return it->second;
            }
            return std::shared_ptr<custom_operation_interpreter>();
        }

        void database::initialize_indexes() {
            add_core_index<dynamic_global_property_index>(*this);
            add_core_index<account_index>(*this);
            add_core_index<account_authority_index>(*this);
            add_core_index<witness_index>(*this);
            add_core_index<transaction_index>(*this);
            add_core_index<block_summary_index>(*this);
            add_core_index<witness_schedule_index>(*this);
            add_core_index<content_index>(*this);
            add_core_index<content_type_index>(*this);
            add_core_index<content_vote_index>(*this);
            add_core_index<witness_vote_index>(*this);
            add_core_index<hardfork_property_index>(*this);
            add_core_index<withdraw_vesting_route_index>(*this);
            add_core_index<owner_authority_history_index>(*this);
            add_core_index<account_recovery_request_index>(*this);
            add_core_index<change_recovery_account_request_index>(*this);
            add_core_index<escrow_index>(*this);
            add_core_index<vesting_delegation_index>(*this);
            add_core_index<vesting_delegation_expiration_index>(*this);
            add_core_index<account_metadata_index>(*this);
            add_core_index<proposal_index>(*this);
            add_core_index<required_approval_index>(*this);
            add_core_index<committee_request_index>(*this);
            add_core_index<committee_vote_index>(*this);
            add_core_index<invite_index>(*this);

            _plugin_index_signal();
        }

        const std::string &database::get_json_schema() const {
            return _json_schema;
        }

        void database::init_schema() {
            /*done_adding_indexes();

   db_schema ds;

   std::vector< std::shared_ptr< abstract_schema > > schema_list;

   std::vector< object_schema > object_schemas;
   get_object_schemas( object_schemas );

   for( const object_schema& oschema : object_schemas )
   {
      ds.object_types.emplace_back();
      ds.object_types.back().space_type.first = oschema.space_id;
      ds.object_types.back().space_type.second = oschema.type_id;
      oschema.schema->get_name( ds.object_types.back().type );
      schema_list.push_back( oschema.schema );
   }

   std::shared_ptr< abstract_schema > operation_schema = get_schema_for_type< operation >();
   operation_schema->get_name( ds.operation_type );
   schema_list.push_back( operation_schema );

   for( const std::pair< std::string, std::shared_ptr< custom_operation_interpreter > >& p : _custom_operation_interpreters )
   {
      ds.custom_operation_types.emplace_back();
      ds.custom_operation_types.back().id = p.first;
      schema_list.push_back( p.second->get_operation_schema() );
      schema_list.back()->get_name( ds.custom_operation_types.back().type );
   }

   graphene::db::add_dependent_schemas( schema_list );
   std::sort( schema_list.begin(), schema_list.end(),
      []( const std::shared_ptr< abstract_schema >& a,
          const std::shared_ptr< abstract_schema >& b )
      {
         return a->id < b->id;
      } );
   auto new_end = std::unique( schema_list.begin(), schema_list.end(),
      []( const std::shared_ptr< abstract_schema >& a,
          const std::shared_ptr< abstract_schema >& b )
      {
         return a->id == b->id;
      } );
   schema_list.erase( new_end, schema_list.end() );

   for( std::shared_ptr< abstract_schema >& s : schema_list )
   {
      std::string tname;
      s->get_name( tname );
      FC_ASSERT( ds.types.find( tname ) == ds.types.end(), "types with different ID's found for name ${tname}", ("tname", tname) );
      std::string ss;
      s->get_str_schema( ss );
      ds.types.emplace( tname, ss );
   }

   _json_schema = fc::json::to_string( ds );
   return;*/
        }

        void database::init_genesis(uint64_t init_supply) {
            try {
                // Create blockchain accounts
                public_key_type initiator_public_key(CHAIN_INITIATOR_PUBLIC_KEY);
                public_key_type committee_public_key(CHAIN_COMMITTEE_PUBLIC_KEY);
                uint32_t bandwidth_reserve_candidates = 1;

                create<account_object>([&](account_object &a) {
                    a.name = CHAIN_NULL_ACCOUNT;
                });
#ifndef IS_LOW_MEM
                create<account_metadata_object>([&](account_metadata_object& m) {
                    m.account = CHAIN_NULL_ACCOUNT;
                });
#endif
                create<account_authority_object>([&](account_authority_object &auth) {
                    auth.account = CHAIN_NULL_ACCOUNT;
                    auth.owner.weight_threshold = 1;
                    auth.active.weight_threshold = 1;
                    auth.posting.weight_threshold = 1;
                });

                create<account_object>([&](account_object &a) {
                    a.name = CHAIN_COMMITTEE_ACCOUNT;
                });
#ifndef IS_LOW_MEM
                create<account_metadata_object>([&](account_metadata_object& m) {
                    m.account = CHAIN_COMMITTEE_ACCOUNT;
                });
#endif
                create<account_authority_object>([&](account_authority_object &auth) {
                    auth.account = CHAIN_COMMITTEE_ACCOUNT;
                    auth.owner.weight_threshold = 1;
                    auth.active.weight_threshold = 1;
                    auth.posting.weight_threshold = 1;
                });
                create<witness_object>([&](witness_object &w) {
                    w.owner = CHAIN_COMMITTEE_ACCOUNT;
                    w.signing_key = committee_public_key;
                    w.schedule = witness_object::top;
                });

                create<account_object>([&](account_object &a) {
                    a.name = CHAIN_ANONYMOUS_ACCOUNT;
                });
#ifndef IS_LOW_MEM
                create<account_metadata_object>([&](account_metadata_object& m) {
                    m.account = CHAIN_ANONYMOUS_ACCOUNT;
                    from_string(m.json_metadata,"0");
                });
#endif
                create<account_authority_object>([&](account_authority_object &auth) {
                    auth.account = CHAIN_ANONYMOUS_ACCOUNT;
                    auth.owner.weight_threshold = 1;
                    auth.active.weight_threshold = 1;
                    auth.posting.weight_threshold = 1;
                });

                // VIZ: invite account
                public_key_type invite_public_key(CHAIN_INVITE_PUBLIC_KEY);
                create<account_object>([&](account_object& a)
                {
                    a.name = CHAIN_INVITE_ACCOUNT;
                    a.balance = asset(0, TOKEN_SYMBOL);
                });
                ++bandwidth_reserve_candidates;

                #ifndef IS_LOW_MEM
                create< account_metadata_object >([&](account_metadata_object& m) {
                    m.account = CHAIN_INVITE_ACCOUNT;
                });
                #endif

                create<account_authority_object>([&](account_authority_object& auth)
                {
                    auth.account = CHAIN_INVITE_ACCOUNT;
                    auth.owner.weight_threshold = 1;
                    auth.active.add_authority( invite_public_key, 1 );
                    auth.active.weight_threshold = 1;
                    auth.posting.weight_threshold = 1;
                });

                if(CHAIN_NUM_INITIATORS>0){
                    for (int i = 0; i < CHAIN_NUM_INITIATORS; ++i) {
                        const auto& name = CHAIN_INITIATOR_NAME + (i ? fc::to_string(i) : std::string());
                        create<account_object>([&](account_object &a) {
                            a.name = name;
                            a.memo_key = initiator_public_key;
                            a.balance = asset(i ? 0 : init_supply, TOKEN_SYMBOL);
                        });
                        ++bandwidth_reserve_candidates;
    #ifndef IS_LOW_MEM
                        create<account_metadata_object>([&](account_metadata_object& m) {
                            m.account = name;
                        });
    #endif
                        create<account_authority_object>([&](account_authority_object &auth) {
                            auth.account = name;
                            auth.owner.add_authority(initiator_public_key, 1);
                            auth.owner.weight_threshold = 1;
                            auth.active = auth.owner;
                            auth.posting = auth.active;
                        });
                        create<witness_object>([&](witness_object &w) {
                            w.owner = name;
                            w.signing_key = initiator_public_key;
                            w.schedule = witness_object::top;
                        });
                    }
                }
                else{
                    create<account_object>([&](account_object &a) {
                        a.name = CHAIN_INITIATOR_NAME;
                        a.memo_key = initiator_public_key;
                        a.balance = asset(init_supply, TOKEN_SYMBOL);
                    });
                    ++bandwidth_reserve_candidates;
#ifndef IS_LOW_MEM
                    create<account_metadata_object>([&](account_metadata_object& m) {
                        m.account = CHAIN_INITIATOR_NAME;
                    });
#endif
                    create<account_authority_object>([&](account_authority_object &auth) {
                        auth.account = CHAIN_INITIATOR_NAME;
                        auth.owner.add_authority(initiator_public_key, 1);
                        auth.owner.weight_threshold = 1;
                        auth.active = auth.owner;
                        auth.posting = auth.active;
                    });
                }

                time_point_sec genesis_time=fc::time_point::now();

                for (int i = 0; i < 0x10000; i++) {
                    create<block_summary_object>([&](block_summary_object &) {});
                }
                create<hardfork_property_object>([&](hardfork_property_object &hpo) {
                    hpo.current_hardfork_version=CHAIN_HARDFORK_STARTUP_VERSION;
                    hpo.processed_hardforks.push_back(genesis_time);
                });

                // Create witness scheduler
                create<witness_schedule_object>([&](witness_schedule_object &wso) {
                    wso.current_shuffled_witnesses[0] = CHAIN_COMMITTEE_ACCOUNT;
                });

                if(CHAIN_STARTUP_HARDFORKS>0){
                    const hardfork_property_object& hardfork_state = get_hardfork_property_object();
                    uint32_t n=0;
                    for(n=0;n<=CHAIN_STARTUP_HARDFORKS;n++){
                        ilog( "Processing ${n} hardfork", ("n", n) );
                        set_hardfork( n, true );
                    }

                    FC_ASSERT( hardfork_state.current_hardfork_version == _hardfork_versions[n], "Unexpected genesis hardfork state" );

                    const auto& witness_idx = get_index<witness_index>().indices().get<by_id>();
                    vector<witness_id_type> wit_ids_to_update;
                    for( auto it=witness_idx.begin(); it!=witness_idx.end(); ++it )
                     wit_ids_to_update.push_back(it->id);

                    for( witness_id_type wit_id : wit_ids_to_update )
                    {
                        modify( get( wit_id ), [&]( witness_object& wit )
                        {
                            wit.running_version = _hardfork_versions[n];
                            wit.hardfork_version_vote = _hardfork_versions[n];
                            wit.hardfork_time_vote = _hardfork_times[n];
                        } );
                    }
                }
                else{
                    const auto& witness_idx = get_index<witness_index>().indices().get<by_id>();
                    vector<witness_id_type> wit_ids_to_update;
                    for( auto it=witness_idx.begin(); it!=witness_idx.end(); ++it )
                     wit_ids_to_update.push_back(it->id);

                    for( witness_id_type wit_id : wit_ids_to_update )
                    {
                        modify( get( wit_id ), [&]( witness_object& wit )
                        {
                            wit.running_version = _hardfork_versions[0];
                            wit.hardfork_version_vote = _hardfork_versions[0];
                            //wit.hardfork_time_vote = genesis_time;
                        } );
                    }
                }

                create<dynamic_global_property_object>([&](dynamic_global_property_object &p) {
                    p.current_witness = CHAIN_COMMITTEE_ACCOUNT;
                    p.recent_slots_filled = fc::uint128_t::max_value();
                    p.participation_count = 128;
                    p.committee_fund = asset(0, TOKEN_SYMBOL);
                    p.current_supply = asset(init_supply, TOKEN_SYMBOL);
                    p.maximum_block_size = CHAIN_BLOCK_SIZE;
                    p.bandwidth_reserve_candidates = bandwidth_reserve_candidates;
                    p.genesis_time=genesis_time;
                });

                /* VIZ Snapshot */
                auto snapshot_json = fc::path(appbase::app().data_dir() / "snapshot.json");
                 if(fc::exists(snapshot_json))
                {
                    share_type init_supply = int64_t( CHAIN_INIT_SUPPLY );
                    ilog("Import snapshot.json");
                    snapshot_items snapshot=fc::json::from_file(snapshot_json).as<snapshot_items>();;
                    for(snapshot_account &account : snapshot.accounts)
                    {
                        public_key_type account_public_key(account.public_key);
                        create< account_object >( [&]( account_object& a )
                        {
                            a.name = account.login;
                            a.memo_key = account_public_key;
                            a.recovery_account = CHAIN_INITIATOR_NAME;
                        } );
                        ++bandwidth_reserve_candidates;

                        create< account_authority_object >( [&]( account_authority_object& auth )
                        {
                            auth.account = account.login;
                            auth.owner.add_authority( account_public_key, 1 );
                            auth.owner.weight_threshold = 1;
                            auth.active  = auth.owner;
                            auth.posting = auth.active;
                            auth.last_owner_update = fc::time_point_sec::min();
                        });
                        #ifndef IS_LOW_MEM
                        create< account_metadata_object >([&](account_metadata_object& m) {
                            m.account = account.login;
                        });
                        #endif
                        create_vesting( get_account( account.login ), asset( account.shares_ammount, TOKEN_SYMBOL ) );
                        init_supply-=account.shares_ammount;

                        ilog( "Import account ${a} with public key ${k}, shares: ${s} (remaining init supply: ${i})", ("a", account.login)("k", account.public_key)("s", account.shares_ammount)("i", init_supply) );
                    }
                    const auto& initiator = get_account( CHAIN_INITIATOR_NAME );
                    modify( initiator, [&]( account_object& a )
                    {
                        a.balance  = asset( init_supply, TOKEN_SYMBOL );
                    } );
                    ilog( "Modify initiator account ${a}, remaining balance: ${i}", ("a", CHAIN_INITIATOR_NAME)("i", init_supply) );
                    const auto &gprops = get_dynamic_global_properties();
                    modify(gprops, [&](dynamic_global_property_object &dgp) {
                        dgp.bandwidth_reserve_candidates = bandwidth_reserve_candidates;
                    });
                }
            }
            FC_CAPTURE_AND_RETHROW()
        }

        uint32_t database::validate_transaction(const signed_transaction &trx, uint32_t skip) {
            const uint32_t validate_transaction_steps =
                skip_authority_check |
                skip_transaction_signatures |
                skip_validate_operations |
                skip_tapos_check;

            // in case of multi-thread application, it's allow to validate transaction in read-thread
            if ((skip & validate_transaction_steps) != validate_transaction_steps) {
                // this method can be used only for push_transaction(),
                //  because such transactions only added to pending list,
                //  and they will be rechecked on block generation
                auto validate_action = [&]() {
                    _validate_transaction(trx, skip);
                };

                if (!(skip & skip_database_locking)) {
                    with_weak_read_lock(std::move(validate_action));
                } else {
                    validate_action();
                }

                skip |= validate_transaction_steps;
            }

            if (!(skip & skip_apply_transaction)) {
                auto apply_action = [&]() {
                    auto session = start_undo_session();
                    _apply_transaction(trx, skip);
                    session.undo();
                };

                if (!(skip & skip_database_locking)) {
                    with_weak_write_lock(std::move(apply_action));
                } else {
                    apply_action();
                }
            }

            return skip;
        }

        void database::_validate_transaction(const signed_transaction &trx, uint32_t skip) {
            if (!(skip & skip_validate_operations)) {   /* issue #505 explains why this skip_flag is disabled */
                trx.validate();
            }

            if (!(skip & (skip_transaction_signatures | skip_authority_check))) {
                const chain_id_type &chain_id = CHAIN_ID;

                auto get_active = [&](const account_name_type& name) {
                    return authority(get<account_authority_object, by_account>(name).active);
                };

                auto get_owner = [&](const account_name_type& name) {
                    return authority(get<account_authority_object, by_account>(name).owner);
                };

                auto get_posting = [&](const account_name_type& name) {
                    return authority(get<account_authority_object, by_account>(name).posting);
                };

                try {
                    trx.verify_authority(chain_id, get_active, get_owner, get_posting, CHAIN_MAX_SIG_CHECK_DEPTH);
                }
                catch (protocol::tx_missing_active_auth &e) {
                    if (get_shared_db_merkle().find(head_block_num() + 1) == get_shared_db_merkle().end()) {
                        throw e;
                    }
                }
            }

            //Skip all manner of expiration and TaPoS checking if we're on block 1;
            // It's impossible that the transaction is expired, and TaPoS makes no sense as no blocks exist.
            if (BOOST_LIKELY(head_block_num() > 0)) {
                if (!(skip & skip_tapos_check)) {
                    const auto &tapos_block_summary = get<block_summary_object>(trx.ref_block_num);
                    //Verify TaPoS block summary has correct ID prefix,
                    //   and that this block's time is not past the expiration
                    FC_ASSERT(
                        trx.ref_block_prefix == tapos_block_summary.block_id._hash[1], "",
                        ("trx.ref_block_prefix", trx.ref_block_prefix)
                        ("tapos_block_summary", tapos_block_summary.block_id._hash[1]));
                }

                fc::time_point_sec now = head_block_time();

                FC_ASSERT(
                    trx.expiration <= now + fc::seconds(CHAIN_MAX_TIME_UNTIL_EXPIRATION), "",
                    ("trx.expiration", trx.expiration)("now", now)
                    ("max_til_exp", CHAIN_MAX_TIME_UNTIL_EXPIRATION));

                FC_ASSERT(now < trx.expiration, "", ("now", now)("trx.exp", trx.expiration));
            }
        }

        void database::notify_changed_objects() {
            try {
                /*vector< graphene::chainbase::generic_id > ids;
      get_changed_ids( ids );
      CHAIN_TRY_NOTIFY( changed_objects, ids )*/
                /*
      if( _undo_db.enabled() )
      {
         const auto& head_undo = _undo_db.head();
         vector<object_id_type> changed_ids;  changed_ids.reserve(head_undo.old_values.size());
         for( const auto& item : head_undo.old_values ) changed_ids.push_back(item.first);
         for( const auto& item : head_undo.new_ids ) changed_ids.push_back(item);
         vector<const object*> removed;
         removed.reserve( head_undo.removed.size() );
         for( const auto& item : head_undo.removed )
         {
            changed_ids.push_back( item.first );
            removed.emplace_back( item.second.get() );
         }
         CHAIN_TRY_NOTIFY( changed_objects, changed_ids )
      }
      */
            }
            FC_CAPTURE_AND_RETHROW()

        }

        void database::set_flush_interval(uint32_t flush_blocks) {
            _flush_blocks = flush_blocks;
            _next_flush_block = 0;
        }

        const block_log &database::get_block_log() const {
            return _block_log;
        }

//////////////////// private methods ////////////////////

        void database::apply_block(const signed_block &next_block, uint32_t skip) {
            try {
                //fc::time_point begin_time = fc::time_point::now();

                auto block_num = next_block.block_num();
                if (_checkpoints.size() &&
                    _checkpoints.rbegin()->second != block_id_type()) {
                    auto itr = _checkpoints.find(block_num);
                    if (itr != _checkpoints.end())
                        FC_ASSERT(next_block.id() ==
                                  itr->second, "Block did not match checkpoint", ("checkpoint", *itr)("block_id", next_block.id()));

                    if (_checkpoints.rbegin()->first >= block_num) {
                        skip = skip_witness_signature
                               | skip_transaction_signatures
                               | skip_transaction_dupe_check
                               | skip_fork_db
                               | skip_block_size_check
                               | skip_tapos_check
                               | skip_authority_check
                               /* | skip_merkle_check While blockchain is being downloaded, txs need to be validated against block headers */
                               | skip_undo_history_check
                               | skip_witness_schedule_check
                               | skip_validate_operations;
                    }
                }

                _apply_block(next_block, skip);

                //fc::time_point end_time = fc::time_point::now();
                //fc::microseconds dt = end_time - begin_time;
                if (_flush_blocks != 0) {
                    if (_next_flush_block == 0) {
                        uint32_t lep = block_num + 1 + _flush_blocks * 9 / 10;
                        uint32_t rep = block_num + 1 + _flush_blocks;

                        // use time_point::now() as RNG source to pick block randomly between lep and rep
                        uint32_t span = rep - lep;
                        uint32_t x = lep;
                        if (span > 0) {
                            uint64_t now = uint64_t(fc::time_point::now().time_since_epoch().count());
                            x += now % span;
                        }
                        _next_flush_block = x;
//                        ilog("Next flush scheduled at block ${b}", ("b", x));
                    }

                    if (_next_flush_block == block_num) {
                        _next_flush_block = 0;
//                        ilog("Flushing database shared memory at block ${b}", ("b", block_num));
                        chainbase::database::flush();
                    }
                }

            } FC_CAPTURE_AND_RETHROW((next_block))
        }

        void database::_apply_block(const signed_block &next_block, uint32_t skip) {
            try {
                uint32_t next_block_num = next_block.block_num();
                const auto &gprops = get_dynamic_global_properties();
                const auto &hardfork_state = get_hardfork_property_object();
                //block_id_type next_block_id = next_block.id();

                _validate_block(next_block, skip);

                const witness_object &signing_witness = validate_block_header(skip, next_block);

                _current_block_num = next_block_num;
                _current_trx_in_block = 0;
                _current_virtual_op = 0;

                /// modify current witness so transaction evaluators can know who included the transaction,
                /// this is mostly for POW operations which must pay the current_witness
                modify(gprops, [&](dynamic_global_property_object &dgp) {
                    dgp.current_witness = next_block.witness;
                });

                if( BOOST_UNLIKELY( next_block_num == 1 ) )//change genesis
                {
                    time_point_sec genesis_time=next_block.timestamp - fc::seconds(CHAIN_BLOCK_INTERVAL);
                    modify(gprops, [&](dynamic_global_property_object &dgp) {
                        dgp.genesis_time=genesis_time;
                    });
                    modify(hardfork_state, [&](hardfork_property_object &hpo) {
                        hpo.processed_hardforks[0]=genesis_time;
                    });
                }

                /// parse witness version reporting
                process_header_extensions(next_block);

                const auto &witness = get_witness(next_block.witness);
                FC_ASSERT(witness.running_version >= hardfork_state.current_hardfork_version,
                        "Block produced by witness that is not running current hardfork",
                        ("witness", witness)("next_block.witness", next_block.witness)("hardfork_state", hardfork_state)
                );

                for (const auto &trx : next_block.transactions) {
                    /* We do not need to push the undo state for each transaction
                     * because they either all apply and are valid or the
                     * entire block fails to apply.  We only need an "undo" state
                     * for transactions when validating broadcast transactions or
                     * when building a block.
                     */
                    apply_transaction(trx, skip);
                    ++_current_trx_in_block;
                }

                _current_trx_in_block = -1;
                _current_op_in_trx = 0;
                _current_virtual_op = 0;

                update_global_dynamic_data(next_block, skip);
                update_signing_witness(signing_witness, next_block);

                update_last_irreversible_block(skip);

                create_block_summary(next_block);
                clear_expired_proposals();
                clear_expired_transactions();
                clear_expired_delegations();
                update_bandwidth_reserve_candidates();
                update_witness_schedule();

                process_funds();
                process_content_cashout();
                process_vesting_withdrawals();

                account_recovery_processing();
                expire_escrow_ratification();

                clear_null_account_balance();
                clear_anonymous_account_balance();
                claim_committee_account_balance();

                committee_processing();
                process_hardforks();

                // notify observers that the block has been applied
                notify_applied_block(next_block);

                notify_changed_objects();
            } FC_CAPTURE_LOG_AND_RETHROW((next_block.block_num()))
        }

        void database::process_header_extensions(const signed_block &next_block) {
            auto itr = next_block.extensions.begin();

            while (itr != next_block.extensions.end()) {
                switch (itr->which()) {
                    case 0: // void_t
                        break;
                    case 1: // version
                    {
                        auto reported_version = itr->get<version>();
                        const auto &signing_witness = get_witness(next_block.witness);
                        //idump( (next_block.witness)(signing_witness.running_version)(reported_version) );

                        if (reported_version !=
                            signing_witness.running_version) {
                            modify(signing_witness, [&](witness_object &wo) {
                                wo.running_version = reported_version;
                            });
                        }
                        break;
                    }
                    case 2: // hardfork_version vote
                    {
                        auto hfv = itr->get<hardfork_version_vote>();
                        const auto &signing_witness = get_witness(next_block.witness);
                        //idump( (next_block.witness)(signing_witness.running_version)(hfv) );

                        if (hfv.hf_version !=
                            signing_witness.hardfork_version_vote ||
                            hfv.hf_time != signing_witness.hardfork_time_vote) {
                            modify(signing_witness, [&](witness_object &wo) {
                                wo.hardfork_version_vote = hfv.hf_version;
                                wo.hardfork_time_vote = hfv.hf_time;
                            });
                        }

                        break;
                    }
                    default:
                        FC_ASSERT(false, "Unknown extension in block header");
                }

                ++itr;
            }
        }

        void database::apply_transaction(const signed_transaction &trx, uint32_t skip) {
            _apply_transaction(trx, skip);
            notify_on_applied_transaction(trx);
        }

        void database::_apply_transaction(const signed_transaction &trx, uint32_t skip) {
            try {
                _current_trx_id = trx.id();
                _current_virtual_op = 0;

                auto &trx_idx = get_index<transaction_index>();
                auto trx_id = trx.id();
                // idump((trx_id)(skip&skip_transaction_dupe_check));
                FC_ASSERT((skip & skip_transaction_dupe_check) ||
                          trx_idx.indices().get<by_trx_id>().find(trx_id) == trx_idx.indices().get<by_trx_id>().end(),
                          "Duplicate transaction check failed", ("trx_ix", trx_id));

                _validate_transaction(trx, skip);

                flat_set<account_name_type> required;
                vector<authority> other;
                trx.get_required_authorities(required, required, required, other);

                auto trx_size = fc::raw::pack_size(trx);

                for (const auto& auth : required) {
                    const auto& acnt = get_account(auth);
                    update_account_bandwidth(acnt, trx_size);
                }

                //Insert transaction into unique transactions database.
                if (!(skip & skip_transaction_dupe_check)) {
                    create<transaction_object>([&](transaction_object &transaction) {
                        transaction.trx_id = trx_id;
                        transaction.expiration = trx.expiration;
                        fc::raw::pack(transaction.packed_trx, trx);
                    });
                }

                //Finally process the operations
                _current_op_in_trx = 0;
                for (const auto &op : trx.operations) {
                    try {
                        apply_operation(op);
                        ++_current_op_in_trx;
                    } FC_CAPTURE_AND_RETHROW((op));
                }
                _current_trx_id = transaction_id_type();

            } FC_CAPTURE_AND_RETHROW((trx))
        }

        void database::apply_operation(const operation &op, bool is_virtual /* false */) {
            operation_notification note(op);
            if (is_virtual) {
                ++_current_virtual_op;
                note.virtual_op = _current_virtual_op;
            }
            notify_pre_apply_operation(note);
            _my->_evaluator_registry.get_evaluator(op).apply(op);
            notify_post_apply_operation(note);
        }

        const witness_object &database::validate_block_header(uint32_t skip, const signed_block &next_block) const {
            try {
                FC_ASSERT(head_block_id() ==
                          next_block.previous, "", ("head_block_id", head_block_id())("next.prev", next_block.previous));
                FC_ASSERT(head_block_time() <
                          next_block.timestamp, "", ("head_block_time", head_block_time())("next", next_block.timestamp)("blocknum", next_block.block_num()));
                const witness_object &witness = get_witness(next_block.witness);

                if (!(skip & skip_witness_signature))
                    FC_ASSERT(next_block.validate_signee(witness.signing_key));

                if (!(skip & skip_witness_schedule_check)) {
                    uint32_t slot_num = get_slot_at_time(next_block.timestamp);
                    FC_ASSERT(slot_num > 0);

                    string scheduled_witness = get_scheduled_witness(slot_num);

                    FC_ASSERT(witness.owner ==
                              scheduled_witness, "Witness produced block at wrong time",
                            ("block witness", next_block.witness)("scheduled", scheduled_witness)("slot_num", slot_num));
                }

                return witness;
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::create_block_summary(const signed_block &next_block) {
            try {
                block_summary_id_type sid(next_block.block_num() & 0xffff);
                modify(get<block_summary_object>(sid), [&](block_summary_object &p) {
                    p.block_id = next_block.id();
                });
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::update_global_dynamic_data(const signed_block &b, uint32_t skip) {
            try {
                auto block_size = fc::raw::pack_size(b);
                const dynamic_global_property_object &_dgp =
                        get_dynamic_global_properties();

                uint32_t missed_blocks = 0;
                if (head_block_time() != fc::time_point_sec()) {
                    missed_blocks = get_slot_at_time(b.timestamp);
                    assert(missed_blocks != 0);
                    missed_blocks--;
                    for (uint32_t i = 0; i < missed_blocks; ++i) {
                        const auto &witness_missed = get_witness(get_scheduled_witness(
                                i + 1));
                        if (witness_missed.owner != b.witness) {
                            modify(witness_missed, [&](witness_object &w) {
                                w.total_missed++;
                                if (head_block_num() -
                                    w.last_confirmed_block_num >
                                    CHAIN_MAX_WITNESS_MISSED_BLOCKS) {
                                    w.signing_key = public_key_type();
                                    push_virtual_operation(shutdown_witness_operation(w.owner));
                                }
                            });
                        }
                    }
                }

                // dynamic global properties updating
                modify(_dgp, [&](dynamic_global_property_object &dgp) {
                    // This is constant time assuming 100% participation. It is O(B) otherwise (B = Num blocks between update)
                    for (uint32_t i = 0; i < missed_blocks + 1; i++) {
                        dgp.participation_count -= dgp.recent_slots_filled.hi & 0x8000000000000000ULL ? 1 : 0;
                        dgp.recent_slots_filled = (dgp.recent_slots_filled << 1) + (i == 0 ? 1 : 0);
                        dgp.participation_count += (i == 0 ? 1 : 0);
                    }

                    dgp.head_block_number = b.block_num();
                    dgp.head_block_id = b.id();
                    dgp.time = b.timestamp;
                    dgp.current_aslot += missed_blocks + 1;
                    dgp.average_block_size =
                            (99 * dgp.average_block_size + block_size) / 100;

       /*
       *  About once per minute the average network use is consulted and used to adjust
       *  the reserve ratio. Anything above 25% usage
       *  reduces the ratio by half which should instantly bring the network from 50% to
       *  25% use unless the demand comes from users who have surplus capacity. In other
       *  words, a 50% reduction in reserve ratio does not result in a 50% reduction in
       *  usage, it will only impact users who where attempting to use more than 50% of
       *  their capacity.
       *
       *  When the reserve ratio is at its max (check CHAIN_MAX_RESERVE_RATIO) a 50%
       *  reduction will take 3 to 4 days to return back to maximum. When it is at its
       *  minimum it will return back to its prior level in just a few minutes.
       *
       *  If the network reserve ratio falls under 100 then it is probably time to
       *  increase the capacity of the network.
       */
                    if (dgp.head_block_number % 20 == 0) {
                        if (dgp.average_block_size > dgp.maximum_block_size / 4) {
                            dgp.current_reserve_ratio /= 2; /// exponential back up
                        } else { /// linear growth... not much fine grain control near full capacity
                            dgp.current_reserve_ratio++;
                        }

                        if (dgp.current_reserve_ratio >
                            CHAIN_MAX_RESERVE_RATIO) {
                            dgp.current_reserve_ratio = CHAIN_MAX_RESERVE_RATIO;
                        }
                    }
                    dgp.max_virtual_bandwidth = (dgp.maximum_block_size *
                                                 dgp.current_reserve_ratio *
                                                 CHAIN_BANDWIDTH_PRECISION *
                                                 CHAIN_BANDWIDTH_AVERAGE_WINDOW_SECONDS) /
                                                CHAIN_BLOCK_INTERVAL;
                });

                if (!(skip & skip_undo_history_check)) {
                    CHAIN_ASSERT(
                        _dgp.head_block_number - _dgp.last_irreversible_block_num < CHAIN_MAX_UNDO_HISTORY,
                        undo_database_exception,
                        "The database does not have enough undo history to support a blockchain with so many missed blocks. "
                        "Please add a checkpoint if you would like to continue applying blocks beyond this point.",
                        ("last_irreversible_block_num", _dgp.last_irreversible_block_num)
                        ("head", _dgp.head_block_number)
                        ("max_undo", CHAIN_MAX_UNDO_HISTORY));
                }
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::update_signing_witness(const witness_object &signing_witness, const signed_block &new_block) {
            try {
                const dynamic_global_property_object &dpo = get_dynamic_global_properties();
                uint64_t new_block_aslot = dpo.current_aslot +
                                           get_slot_at_time(new_block.timestamp);

                modify(signing_witness, [&](witness_object &_wit) {
                    _wit.last_aslot = new_block_aslot;
                    _wit.last_confirmed_block_num = new_block.block_num();
                });
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::update_last_irreversible_block(uint32_t skip) {
            try {
                const dynamic_global_property_object &dpo = get_dynamic_global_properties();
                const witness_schedule_object &wso = get_witness_schedule_object();

                vector<const witness_object *> wit_objs;
                wit_objs.reserve(wso.num_scheduled_witnesses);
                for (int i = 0; i < wso.num_scheduled_witnesses; i+=CHAIN_BLOCK_WITNESS_REPEAT) {
                    wit_objs.push_back(&get_witness(wso.current_shuffled_witnesses[i]));
                }

                static_assert(CHAIN_IRREVERSIBLE_THRESHOLD >
                              0, "irreversible threshold must be nonzero");

                // 1 1 1 2 2 2 2 2 2 2 -> 2     .7*10 = 7
                // 1 1 1 1 1 1 1 2 2 2 -> 1
                // 3 3 3 3 3 3 3 3 3 3 -> 3

                size_t offset = ((CHAIN_100_PERCENT -
                                  CHAIN_IRREVERSIBLE_THRESHOLD) *
                                 wit_objs.size() / CHAIN_100_PERCENT);

                std::nth_element(wit_objs.begin(),
                        wit_objs.begin() + offset, wit_objs.end(),
                        [](const witness_object *a, const witness_object *b) {
                            return a->last_confirmed_block_num <
                                   b->last_confirmed_block_num;
                        });

                uint32_t new_last_irreversible_block_num = wit_objs[offset]->last_confirmed_block_num;

                if (new_last_irreversible_block_num >
                    dpo.last_irreversible_block_num) {
                    modify(dpo, [&](dynamic_global_property_object &_dpo) {
                        _dpo.last_irreversible_block_num = new_last_irreversible_block_num;
                    });
                }

                commit(dpo.last_irreversible_block_num);

                if (!(skip & skip_block_log)) {
                    // output to block log based on new last irreverisible block num
                    const auto &tmp_head = _block_log.head();
                    uint64_t log_head_num = 0;

                    if (tmp_head) {
                        log_head_num = tmp_head->block_num();
                    }

                    if (log_head_num < dpo.last_irreversible_block_num) {
                        while (log_head_num < dpo.last_irreversible_block_num) {
                            std::shared_ptr<fork_item> block = _fork_db.fetch_block_on_main_branch_by_number(
                                    log_head_num + 1);
                            FC_ASSERT(block, "Current fork in the fork database does not contain the last_irreversible_block");
                            _block_log.append(block->data);
                            log_head_num++;
                        }

                        _block_log.flush();
                    }
                }

                _fork_db.set_max_size(dpo.head_block_number -
                                      dpo.last_irreversible_block_num + 1);
            } FC_CAPTURE_AND_RETHROW()
        }

        void database::clear_expired_transactions() {
            //Look for expired transactions in the deduplication list, and remove them.
            //Transactions must have expired by at least two forking windows in order to be removed.
            auto &transaction_idx = get_index<transaction_index>();
            const auto &dedupe_index = transaction_idx.indices().get<by_expiration>();
            while ((!dedupe_index.empty()) &&
                   (head_block_time() > dedupe_index.begin()->expiration)) {
                remove(*dedupe_index.begin());
            }
        }

        void database::clear_expired_delegations() {
            auto now = head_block_time();
            const auto& delegations_by_exp = get_index<vesting_delegation_expiration_index, by_expiration>();
            auto itr = delegations_by_exp.begin();
            while (itr != delegations_by_exp.end() && itr->expiration < now) {
                modify(get_account(itr->delegator), [&](account_object& a) {
                    a.delegated_vesting_shares -= itr->vesting_shares;
                });
                push_virtual_operation(return_vesting_delegation_operation(itr->delegator, itr->vesting_shares));
                remove(*itr);
                itr = delegations_by_exp.begin();
            }
        }

        void database::adjust_balance(const account_object &a, const asset &delta) {
            modify(a, [&](account_object &acnt) {
                switch (delta.symbol) {
                    case TOKEN_SYMBOL:
                        acnt.balance += delta;
                        break;
                    default:
                        FC_ASSERT(false, "invalid symbol");
                }
            });
        }


        void database::burn_asset(const asset &delta) {
            const auto &props = get_dynamic_global_properties();
            modify(props, [&](dynamic_global_property_object &props) {
                switch (delta.symbol) {
                    case TOKEN_SYMBOL: {
                        props.current_supply += delta;
                        assert(props.current_supply.amount.value >= 0);
                        break;
                    }
                    default:
                        FC_ASSERT(false, "invalid symbol");
                }
            });
        }


        asset database::get_balance(const account_object &a, asset_symbol_type symbol) const {
            switch (symbol) {
                case TOKEN_SYMBOL:
                    return a.balance;
                default:
                    FC_ASSERT(false, "invalid symbol");
            }
        }


        void database::init_hardforks() {
            const dynamic_global_property_object &dpo = get_dynamic_global_properties();
            _hardfork_times[0] = dpo.genesis_time;
            _hardfork_versions[0] = hardfork_version(CHAIN_STARTUP_VERSION);

            /* HARDFORK EXAMPLE */
            /* HF 1 TIME AND VERSION */
            /*
            _hardfork_times[CHAIN_HARDFORK_1] = fc::time_point_sec(CHAIN_HARDFORK_1_TIME);
            _hardfork_versions[CHAIN_HARDFORK_1] = CHAIN_HARDFORK_1_VERSION;
            */

            const auto &hardforks = get_hardfork_property_object();
            FC_ASSERT(
                hardforks.last_hardfork <= CHAIN_NUM_HARDFORKS,
                "Chain knows of more hardforks than configuration",
                ("hardforks.last_hardfork", hardforks.last_hardfork)
                ("CHAIN_NUM_HARDFORKS", CHAIN_NUM_HARDFORKS));
            FC_ASSERT(
                _hardfork_versions[hardforks.last_hardfork] <= CHAIN_VERSION,
                "Blockchain version is older than last applied hardfork");
            FC_ASSERT(CHAIN_HARDFORK_VERSION == _hardfork_versions[CHAIN_NUM_HARDFORKS]);
        }

        void database::reset_virtual_schedule_time() {
            const witness_schedule_object &wso = get_witness_schedule_object();
            modify(wso, [&](witness_schedule_object &o) {
                o.current_virtual_time = fc::uint128_t(); // reset it 0
            });

            const auto &idx = get_index<witness_index>().indices();
            for (const auto &witness : idx) {
                modify(witness, [&](witness_object &wobj) {
                    wobj.virtual_position = fc::uint128_t();
                    wobj.virtual_last_update = wso.current_virtual_time;
                    wobj.virtual_scheduled_time = VIRTUAL_SCHEDULE_LAP_LENGTH2 /
                                                  (wobj.votes.value + 1);
                });
            }
        }

        void database::process_hardforks() {
            try {
                // If there are upcoming hardforks and the next one is later, do nothing
                const auto &hardforks = get_hardfork_property_object();

                while (_hardfork_versions[hardforks.last_hardfork] <
                       hardforks.next_hardfork
                       &&
                       hardforks.next_hardfork_time <= head_block_time()) {
                    if (hardforks.last_hardfork < CHAIN_NUM_HARDFORKS) {
                        apply_hardfork(hardforks.last_hardfork + 1);
                    } else {
                        throw unknown_hardfork_exception();
                    }
                }
            }
            FC_CAPTURE_AND_RETHROW()
        }

        bool database::has_hardfork(uint32_t hardfork) const {
            return get_hardfork_property_object().processed_hardforks.size() >
                   hardfork;
        }

        void database::set_hardfork(uint32_t hardfork, bool apply_now) {
            auto const &hardforks = get_hardfork_property_object();

            for (uint32_t i = hardforks.last_hardfork + 1;
                 i <= hardfork && i <= CHAIN_NUM_HARDFORKS; i++) {
                modify(hardforks, [&](hardfork_property_object &hpo) {
                    hpo.next_hardfork = _hardfork_versions[i];
                    hpo.next_hardfork_time = head_block_time();
                });

                if (apply_now) {
                    apply_hardfork(i);
                }
            }
        }

        void database::apply_hardfork(uint32_t hardfork) {
            if (_log_hardforks) {
                elog("HARDFORK ${hf} at block ${b}", ("hf", hardfork)("b", head_block_num()));
            }

            switch (hardfork) {
                /* HARDFORK EXAMPLE */
                /*
                case CHAIN_HARDFORK_1:
                    break;
                */
                default:
                    break;
            }

            modify(get_hardfork_property_object(), [&](hardfork_property_object &hfp) {
                FC_ASSERT(hardfork == hfp.last_hardfork +
                                      1, "Hardfork being applied out of order", ("hardfork", hardfork)("hfp.last_hardfork", hfp.last_hardfork));
                FC_ASSERT(hfp.processed_hardforks.size() ==
                          hardfork, "Hardfork being applied out of order");
                hfp.processed_hardforks.push_back(_hardfork_times[hardfork]);
                hfp.last_hardfork = hardfork;
                hfp.current_hardfork_version = _hardfork_versions[hardfork];
                FC_ASSERT(hfp.processed_hardforks[hfp.last_hardfork] ==
                          _hardfork_times[hfp.last_hardfork], "Hardfork processing failed sanity check...");
            });

            push_virtual_operation(hardfork_operation(hardfork), true);
        }

} } //graphene::chain
