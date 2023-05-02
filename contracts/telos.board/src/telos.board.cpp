#include <telos.board.hpp>
#include <eosio/symbol.hpp>

tfvt::tfvt(name self, name code, datastream<const char*> ds)
: contract(self, code, ds),
  configs(get_self(), get_self().value),
  seats(get_self(), get_self().value) {
	print("\n exists?: ", configs.exists());
	_config = configs.exists() ? configs.get() : get_default_config();
}

tfvt::~tfvt() {
	if(configs.exists()) configs.set(_config, get_self());
}

tfvt::configv2 tfvt::get_default_config() {
	auto c = configv2 {
		get_self(),			//publisher
		name(),				//open_election_id
		uint32_t(5), 		//holder_quorum_divisor
		uint32_t(2), 		//board_quorum_divisor
		uint32_t(2000000),	//issue_duration
		uint32_t(1200),  	//start_delay
		uint32_t(2000000),  //leaderboard_duration
		uint32_t(14515200),	//election_frequency
		uint32_t(0),		//active election min time to start
		false				//is_active_election
	};
	configs.set(c, get_self());
	return c;
}

#pragma region Actions

void tfvt::setconfig(name member, configv2 new_config) {
    require_auth(get_self());
	configs.remove();
	check(new_config.holder_quorum_divisor > 0, "holder_quorum_divisor must be a non-zero number");
	check(new_config.board_quorum_divisor > 0, "board_quorum_divisor must be a non-zero number");
	check(new_config.issue_duration > 0, "issue_duration must be a non-zero number");
	check(new_config.start_delay > 0, "start_delay must be a non-zero number");
	check(new_config.leaderboard_duration > 0, "leaderboard_duration must be a non-zero number");
	check(new_config.election_frequency > 0, "election_frequency must be a non-zero number");

	new_config.publisher = _config.publisher;
	new_config.open_election_id = _config.open_election_id;
	new_config.is_active_election = _config.is_active_election;

	_config = new_config;
	configs.set(_config, get_self());
}

void tfvt::nominate(name nominee, name nominator) {
    require_auth(nominator);
    check_nominee(nominee);

    nominees_table noms(get_self(), get_self().value);
    auto n = noms.find(nominee.value);
    check(n == noms.end(), "nominee has already been nominated");

    noms.emplace(get_self(), [&](auto& m) {
        m.nominee = nominee;
    });
}

void tfvt::makeelection(name holder, std::string description, std::string content) {
	require_auth(holder);
	check(!_config.is_active_election, "there is already an election in progress");
	check(get_open_seats() > 0, "It isn't time for the next election");

	ballots_table ballots(TELOS_DECIDE_N, TELOS_DECIDE_N.value);

	_config.open_election_id = get_next_ballot_id();

	_config.active_election_min_start_time = current_time_point().sec_since_epoch() + _config.start_delay;

    action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("newballot"), make_tuple(
		name(_config.open_election_id), // ballot name
		name("leaderboard"), // type
		get_self(), // publisher
		symbol("VOTE", 4), // treasury symbol
		name("1tokennvote"), // voting method
		vector<name>() // initial options
	)).send();

	// blindly toggling votestake
	action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("togglebal"), make_tuple(
		name(_config.open_election_id), // ballot name
		name("votestake") // setting name
	)).send();

	action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("editdetails"), make_tuple(
		name(_config.open_election_id), // ballot name
		std::string("TF Board Election"), // title
		description,
		content
	)).send();

    // Remove all the expired seats
    for (auto itr = seats.begin(); itr != seats.end(); itr++) {
        if (is_term_expired(itr->next_election_time))  {
            seats.modify(itr, get_self(), [&](auto& s) {
                s.member = name();
            });
        }
    }

	//NOTE: this prevents makeelection from being called multiple times.
	//NOTE2 : this gets overwritten by setconfig
	_config.is_active_election = true;
}

void tfvt::addcand(name candidate) {
	require_auth(candidate);
	check(is_nominee(candidate), "only nominees can be added to the election");
	check(_config.is_active_election, "no active election for board members at this time");

	auto seat = get_board_seat_by_user(candidate);

	check(seat == seats.end() || is_term_expired(seat->next_election_time), "nominee can't already be a board member, or their term must be expired.");

    action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("addoption"), make_tuple(
		_config.open_election_id, 	//ballot_id
		candidate 					//new_candidate
	)).send();
}

void tfvt::removecand(name candidate) {
	require_auth(candidate);
	check(is_nominee(candidate), "candidate is not a nominee");

    action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("rmvoption"), make_tuple(
		_config.open_election_id, 	//ballot_id
		candidate 					//new_candidate
	)).send();
}

void tfvt::startelect(name holder) {
	require_auth(holder);
	check(_config.is_active_election, "there is no active election to start");
	check(current_time_point().sec_since_epoch() > _config.active_election_min_start_time, "It isn't time to start the election");

	uint32_t election_end_time = current_time_point().sec_since_epoch() + _config.leaderboard_duration;

    uint8_t min = 1;
    uint8_t max = get_open_seats();

    action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("editminmax"), make_tuple(
            name(_config.open_election_id), // ballot name
            min, // new_min_options
            max // new_min_options
    )).send();

	action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("openvoting"), make_tuple(
		_config.open_election_id, 	//ballot_id
		election_end_time
	)).send();
}

void tfvt::endelect(name holder) {
    require_auth(holder);
	check(_config.is_active_election, "there is no active election to end");
	uint8_t status = 1;

    ballots_table ballots(TELOS_DECIDE_N, TELOS_DECIDE_N.value);
    auto bal = ballots.get(_config.open_election_id.value);
	map<name, asset> candidates = bal.options;
	vector<pair<int64_t, name>> sorted_candidates;

	for (auto it = candidates.begin(); it != candidates.end(); ++it) {
		sorted_candidates.push_back(make_pair(it->second.amount, it->first));
	}
	sort(sorted_candidates.begin(), sorted_candidates.end(), [](const auto &c1, const auto &c2) { return c1 > c2; });

    size_t open_seats = get_open_seats();
	// Remove candidates tied with the [available-seats] - This discards all the tied on the tail
	if (sorted_candidates.size() > open_seats) {
		auto first_cand_out = sorted_candidates[open_seats];
		sorted_candidates.resize(open_seats);

		// count candidates that are tied with first_cand_out
		uint8_t tied_cands = 0;
		for(int i = sorted_candidates.size() - 1; i >= 0; i--) {
			if(sorted_candidates[i].first == first_cand_out.first) {
				tied_cands++;
			}
		}

		// remove all tied candidates
		if(tied_cands > 0) {
			sorted_candidates.resize(sorted_candidates.size() - tied_cands);
		}
	}

    for (int n = 0; n < sorted_candidates.size(); n++) {
		if(sorted_candidates[n].first > 0) {
			add_to_tfboard(sorted_candidates[n].second);
		}
    }

	vector<permission_level_weight> currently_elected = perms_from_members(); //NOTE: needs testing

	if(currently_elected.size() > 0)
		set_permissions(currently_elected);

	action(permission_level{get_self(), name("active")}, TELOS_DECIDE_N, name("closevoting"), make_tuple(
		_config.open_election_id,
		false
	)).send();
	_config.is_active_election = false;
}

void tfvt::removemember(name member_to_remove) {
	require_auth(get_self());

	remove_and_seize(member_to_remove);

	auto perms = perms_from_members();
	set_permissions(perms);
}

void tfvt::resign(name member) {
	require_auth(member);

	remove_and_seize(member);

	auto perms = perms_from_members();
	set_permissions(perms);
}

void tfvt::removeseat(uint32_t seat_id) {
    require_auth(get_self());

    auto seat = seats.find(seat_id);
    check(seat != seats.end(), "Unknown seat");
    check(is_empty_seat(seat), "Seat is not empty");
    seats.erase(seat);
}

void tfvt::updseatterms(std::map<uint32_t, uint32_t> seat_terms) {
    require_auth(get_self());

    for (auto const& it : seat_terms) {
        auto seat = seats.find(it.first);
        check(seat != seats.end(), "Unknown seat");
        seats.modify(seat, get_self(), [&](auto& s) {
            s.next_election_time = it.second;
        });
    }
}

void tfvt::migratestart() {
    require_auth(get_self());

    members_table members(get_self(), get_self().value);
    config_table_old configs_old(get_self(), get_self().value);

    check(configs_old.exists(), "No config to migrate from");
    config config_old = configs_old.get();

    // Move config
    _config.publisher = config_old.publisher;
    _config.open_election_id = config_old.open_election_id;
    _config.holder_quorum_divisor = config_old.holder_quorum_divisor;
    _config.board_quorum_divisor = config_old.board_quorum_divisor;
    _config.issue_duration = config_old.issue_duration;
    _config.start_delay = config_old.start_delay;
    _config.leaderboard_duration = config_old.leaderboard_duration;
    _config.election_frequency = config_old.election_frequency;
    _config.active_election_min_start_time = config_old.active_election_min_start_time;
    _config.is_active_election = config_old.is_active_election;

    // Clean sets
    auto it = seats.begin();
    while(it != seats.end()) {
        it = seats.erase(it);
    }

    // Move members to a seat
    for (auto const& it : members) {
        seats.emplace(get_self(), [&](auto& s) {
            s.id = seats.available_primary_key();
            s.member = it.member;
            s.next_election_time = config_old.last_board_election_time + config_old.election_frequency;
        });
    }
}

void tfvt::migrateclean() {
    require_auth(get_self());

    config_table_old configs_old(get_self(), get_self().value);

    // Delete old config
    configs_old.remove();

    // Delete members
    members_table members(get_self(), get_self().value);
    auto it = members.begin();
    while(it != members.end()) {
        it = members.erase(it);
    }
}

#pragma endregion Actions


#pragma region Helper_Functions

void tfvt::add_to_tfboard(name nominee) {
    nominees_table noms(get_self(), get_self().value);
    auto n = noms.find(nominee.value);
    check(n != noms.end(), "nominee doesn't exist in table");
    auto seat = get_next_empty_seat();
    seats.modify(seat, get_self(), [&](auto& s) {
        s.member = nominee;
        if (is_term_expired(s.next_election_time)) {
            s.next_election_time += _config.election_frequency;
        }
    });

    noms.erase(n);
}

void tfvt::addseats(uint8_t num_seats) {
    require_auth(get_self());

    for (size_t i = 0; i < num_seats; ++i) {
        seats.emplace(get_self(), [&](auto& s) {
            s.id = seats.available_primary_key();
            s.member = name();
            s.next_election_time = current_time_point().sec_since_epoch();
        });
    }
}

bool tfvt::is_board_member(name user) {
    auto seat = get_board_seat_by_user(user);

    return seat != seats.end();
}

tfvt::seats_table::const_iterator tfvt::get_board_seat_by_user(name user) {
    for (auto seat = seats.begin(); seat != seats.end(); seat++) {
        if (seat->member == user) {
            return seat;
        }
    }

    return seats.end();
}

bool tfvt::is_nominee(name user) {
    nominees_table noms(get_self(), get_self().value);
    auto n = noms.find(user.value);

    return n != noms.end();
}

bool tfvt::is_term_expired(uint32_t next_election_time) {
    return current_time_point().sec_since_epoch() >= next_election_time;
}

void tfvt::remove_and_seize(name member) {

	auto seat = get_board_seat_by_user(member);
	check(seat != seats.end(), "board member not found");

    seats.modify(seat, get_self(), [&](auto& s) {
        s.member = name();
    });
}

void tfvt::set_permissions(vector<permission_level_weight> perms) {
	auto self = get_self();
	uint16_t active_weight = perms.size() < 3 ? 1 : ((perms.size() / 3) * 2);

	perms.emplace_back(
		permission_level_weight{ permission_level{
				self,
				"eosio.code"_n
			}, active_weight}
	);
	sort(perms.begin(), perms.end(), [](const auto &first, const auto &second) { return first.permission.actor.value < second.permission.actor.value; });

	action(permission_level{get_self(), "owner"_n }, "eosio"_n, "updateauth"_n,
		std::make_tuple(
			get_self(),
			name("active"),
			name("owner"),
			authority {
				active_weight,
				std::vector<key_weight>{},
				perms,
				std::vector<wait_weight>{}
			}
		)
	).send();

	auto tf_it = std::find_if(perms.begin(), perms.end(), [&self](const permission_level_weight &lvlw) {
        return lvlw.permission.actor == self;
    });
	perms.erase(tf_it);
	uint16_t minor_weight = perms.size() < 4 ? 1 : (perms.size() / 4);
	action(permission_level{get_self(), "owner"_n }, "eosio"_n, "updateauth"_n,
		std::make_tuple(
			get_self(),
			name("minor"),
			name("owner"),
			authority {
				minor_weight,
				std::vector<key_weight>{},
				perms,
				std::vector<wait_weight>{}
			}
		)
	).send();
}

vector<tfvt::permission_level_weight> tfvt::perms_from_members() {
	auto itr = seats.begin();

	vector<permission_level_weight> perms;
	while(itr != seats.end()) {
	    if (!is_empty_seat(itr)) { // Only members from non empty seats are taken into account
	        perms.emplace_back(permission_level_weight{ permission_level{
                itr->member,
                "active"_n
            }, 1});
	    }
		itr++;
	}

	return perms;
}

name tfvt::get_next_ballot_id() {
	name ballot_id = _config.open_election_id;
	if (ballot_id == name()) {
		ballot_id = name("tfvt.");
	}

	ballots_table ballots(TELOS_DECIDE_N, TELOS_DECIDE_N.value);
	// Check 500 ballots ahead
	for (size_t i = 0; i < 500; ++i) {
		ballot_id = name(ballot_id.value + 1);
		auto bal = ballots.find(ballot_id.value);
		if (bal == ballots.end()) {
			return ballot_id;
		}
	}

	check(false, "couldn't secure a ballot_id");
	// silence the compiler
	return name();
}

size_t tfvt::get_open_seats() {
    // Gets open seats
    // An open seat is one that:
    //   - Has no member OR
    //   - next_election_time is in the past
    int open_seats = 0;

    for (auto seat = seats.begin(); seat != seats.end(); seat++) {
        if (is_empty_seat(seat)) {
            open_seats++;
        }
    }

    return open_seats;
}

void tfvt::check_nominee(name nominee) {
    check(is_account(nominee), "nominee account must exist");
    if (is_board_member(nominee)) {
        auto seat = get_board_seat_by_user(nominee);
        check(is_term_expired(seat->next_election_time), "nominee is a board member, nominee's term must be expired");
    }
}

tfvt::seats_table::const_iterator tfvt::get_next_empty_seat() {
    for (auto seat = seats.begin(); seat != seats.end(); seat++) {
        if (is_empty_seat(seat)) {
            return seat;
        }
    }

    check(false, "No empty seat remaining - this is likely a bug");
    return seats.end(); // silence warning
}

bool tfvt::is_empty_seat(seats_table::const_iterator& seat) {
    return seat->member == name() || is_term_expired(seat->next_election_time);
}

#pragma endregion Helper_Functions
