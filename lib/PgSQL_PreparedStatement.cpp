#include "proxysql.h"
#include "cpp.h"

#ifndef SPOOKYV2
#include "SpookyV2.h"
#define SPOOKYV2
#endif

#include "PgSQL_PreparedStatement.h"
#include "PgSQL_Protocol.h"

extern PgSQL_STMT_Manager_v14 *GloPgStmt;

const int PS_GLOBAL_STATUS_FIELD_NUM = 8;

static uint64_t stmt_compute_hash(const char *user,
	const char *database, const char *query, unsigned int query_length, const Parse_Param_Types& param_types) {
	// two random seperators
	static const char DELIM1[] = "-ZiODNjvcNHTFaARXoqqSPDqQe-";
	static const char DELIM2[] = "-aSfpWDoswfuRsJXqZKfcelzCL-";
	static const char DELIM3[] = "-rQkhRVXdvgVYsmiqZCMikjKmP-";

	// NOSONAR: strlen is safe here 
	size_t user_length = strlen(user); // NOSONAR
	// NOSONAR: strlen is safe here 
	size_t database_length = strlen(database); // NOSONAR
	size_t delim1_length = sizeof(DELIM1) - 1;
	size_t delim2_length = sizeof(DELIM2) - 1;
	size_t delim3_length = sizeof(DELIM3) - 1;

	size_t l = 0;
	l += user_length;
	l += database_length;
	l += delim1_length;
	l += delim2_length;
	l += query_length;
	if (!param_types.empty()) {
		l += delim3_length; // add length for the third delimiter
		l += sizeof(uint16_t); // add length for number of parameter types
		l += (param_types.size() * sizeof(uint32_t)); // add length for parameter types
	}

	std::vector<char> storage(l);
	char* buf = storage.data();
	l = 0;
	memcpy(buf + l, user, user_length);		l += user_length;		// write user
	memcpy(buf + l, DELIM1, delim1_length); l += delim1_length; // write delimiter1
	memcpy(buf + l, database, database_length); l += database_length; // write database
	memcpy(buf + l, DELIM2, delim2_length); l += delim2_length; // write delimiter2
	memcpy(buf + l, query, query_length);	l += query_length; 	// write query
	if (!param_types.empty()) {
		uint16_t size = param_types.size();
		memcpy(buf + l, DELIM3, delim3_length); l += delim3_length; // write delimiter3
		memcpy(buf + l, &size, sizeof(uint16_t)); l += sizeof(uint16_t); // write number of parameter types
		memcpy(buf + l, param_types.data(), size * sizeof(uint32_t)); l += (size * sizeof(uint32_t)); // write each parameter type
	}
	uint64_t hash = SpookyHash::Hash64(buf, l, 0);
	return hash;
}

void PgSQL_STMT_Global_info::compute_hash() {
	hash = stmt_compute_hash(username, dbname, query,
		query_length, parse_param_types);
}

PgSQL_STMT_Global_info::PgSQL_STMT_Global_info(uint64_t id,
                                               char *u, char *d, char *q,
                                               unsigned int ql,
                                               char *fc,
											   Parse_Param_Types&& ppt,
                                               uint64_t _h) {
	total_mem_usage = 0;
	statement_id = id;
	ref_count_client = 0;
	ref_count_server = 0;
	digest_text = nullptr;
	username = strdup(u);
	dbname = strdup(d);
	query = (char *)malloc(ql + 1);
	memcpy(query, q, ql);
	query[ql] = '\0';  // add NULL byte
	query_length = ql;
	first_comment = fc ? strdup(fc) : nullptr;
	parse_param_types = std::move(ppt);
	PgQueryCmd = PGSQL_QUERY__UNINITIALIZED;
	
	if (_h) {
		hash = _h;
	} else {
		compute_hash();
	}
	calculate_mem_usage();
}

PgSQL_STMT_Global_info::~PgSQL_STMT_Global_info() {
	free(username);
	free(dbname);
	free(query);
	if (first_comment)
		free(first_comment);
	if (digest_text)
		free(digest_text);
	parse_param_types.clear(); // clear the parameter types vector
}

void PgSQL_STMT_Global_info::calculate_mem_usage() {
	total_mem_usage = sizeof(PgSQL_STMT_Global_info) +
		query_length + 1;

	// NOSONAR: strlen is safe here 
	if (username) total_mem_usage += strlen(username) + 1; // NOSONAR
	if (dbname) total_mem_usage += strlen(dbname) + 1; // NOSONAR
	if (first_comment) total_mem_usage += strlen(first_comment) + 1; // NOSONAR
	if (digest_text) total_mem_usage += strlen(digest_text) + 1; // NOSONAR
}

void PgSQL_STMTs_local_v14::backend_insert(uint64_t global_stmt_id, uint32_t backend_stmt_id) {
	global_stmt_to_backend_ids.insert(std::make_pair(global_stmt_id, backend_stmt_id));
	backend_stmt_to_global_ids.insert(std::make_pair(backend_stmt_id,global_stmt_id));
}

void PgSQL_STMTs_local_v14::client_insert(PgSQL_STMT_Global_info* stmt_info, const std::string& client_stmt_name, bool ref_client_inc, std::map<uint32_t, uint64_t>::iterator it) {

	uint64_t global_stmt_id = stmt_info->statement_id;

	if (it != stmt_name_to_global_ids.end()) {
		uint64_t existing_global_id = it->second;

		if (existing_global_id == global_stmt_id) {
			// Already mapped to same global id — nothing to do
#ifdef DEBUG
			bool found = false;
			auto range = global_id_to_stmt_names.equal_range(global_stmt_id);
			for (auto iter = range.first; iter != range.second; ++iter) {
				if (iter->second == client_stmt_name) { found = true; break; }
			}
			assert(found && "Inconsistent: missing mapping in global_id_to_stmt_names");
#endif
			if (ref_client_inc)
				GloPgStmt->ref_count_client(stmt_info, -1);
			return;
		}

		// Update existing entry to new global id
		it->second = global_stmt_id;

		auto range = global_id_to_stmt_names.equal_range(existing_global_id);
		// Remove old mapping in global_id_to_stmt_names
		for (auto iter = range.first; iter != range.second; ++it) {
			if (iter->second == client_stmt_name) {
				iter = global_id_to_stmt_names.erase(iter);
				break;
			}
		}

		// Add new mapping
		global_id_to_stmt_names.emplace(global_stmt_id, client_stmt_name);

		// Adjust refcounts: decrement old, increment new
		GloPgStmt->ref_count_client(existing_global_id, -1);

		if (ref_client_inc == false)
			GloPgStmt->ref_count_client(stmt_info, 1);
		return;
	}

	// New statement name — just insert
	uint32_t hash = SpookyHash::Hash32(client_stmt_name.data(), client_stmt_name.size(), 0);
	stmt_name_to_global_ids.emplace(hash, global_stmt_id);
	global_id_to_stmt_names.emplace(global_stmt_id, client_stmt_name);

	if (ref_client_inc == false)
		GloPgStmt->ref_count_client(stmt_info, 1);
}

uint64_t PgSQL_STMTs_local_v14::compute_hash(const char *user,
	const char *database, const char *query, unsigned int query_length, const Parse_Param_Types& param_types) {
	uint64_t hash = stmt_compute_hash(user, database, query, query_length, param_types);
	return hash;
}

PgSQL_STMT_Manager_v14::PgSQL_STMT_Manager_v14() {
	last_purge_time = time(NULL);
	pthread_rwlock_init(&rwlock_, NULL);
	next_statement_id = 1;  // we initialize this as 1 because we 0 is not allowed
	num_stmt_with_ref_client_count_zero = 0;
	num_stmt_with_ref_server_count_zero = 0;
	statuses.c_unique = 0;
	statuses.c_total = 0;
	statuses.stmt_max_stmt_id = 0;
	statuses.cached = 0;
	statuses.s_unique = 0;
	statuses.s_total = 0;
}

PgSQL_STMT_Manager_v14::~PgSQL_STMT_Manager_v14() {
	wrlock();
	for (auto it = map_stmt_id_to_info.begin(); it != map_stmt_id_to_info.end(); ++it) {
		PgSQL_STMT_Global_info * a = it->second;
		delete a;
	}
	unlock();
}

void PgSQL_STMT_Manager_v14::purge_stmts_if_needed(bool is_locked) noexcept {
	time_t ct = time(NULL);
	if (ct <= last_purge_time + 1)
		return; // too soon, skip

	// --- Light pre-check without lock ---
	size_t map_size = map_stmt_id_to_info.size();
	uint64_t num_client_zero = num_stmt_with_ref_client_count_zero.load(std::memory_order_relaxed);
	uint64_t num_server_zero = num_stmt_with_ref_server_count_zero.load(std::memory_order_relaxed);

	if (map_size <= (unsigned)pgsql_thread___max_stmts_cache ||
		num_client_zero <= map_size / 10 ||
		num_server_zero <= map_size / 10)
	{
		// Heuristic says no purge needed
		return;
	}

	if (is_locked)
		unlock();

	// --- Now we know we might purge, take write lock ---
	wrlock();

	// Double-check under exclusive lock (authoritative)
	ct = time(NULL);
	if (ct <= last_purge_time + 1) {
		unlock();
		return;
	}

	map_size = map_stmt_id_to_info.size();
	num_client_zero = num_stmt_with_ref_client_count_zero.load(std::memory_order_relaxed);
	num_server_zero = num_stmt_with_ref_server_count_zero.load(std::memory_order_relaxed);

	if (map_size <= (unsigned)pgsql_thread___max_stmts_cache ||
		num_client_zero <= map_size / 10 ||
		num_server_zero <= map_size / 10)
	{
		last_purge_time = ct;
		unlock();
		return;
	}

	// --- Actual purge happens here under wrlock() ---
	last_purge_time = ct;

	std::vector<uint64_t> to_remove;
	to_remove.reserve(map_size);

	for (auto it = map_stmt_id_to_info.begin(); it != map_stmt_id_to_info.end(); ++it) {
		PgSQL_STMT_Global_info* a = it->second;
		int rc_client = a->ref_count_client.load(std::memory_order_acquire);
		int rc_server = a->ref_count_server.load(std::memory_order_acquire);

		if (rc_client == 0 && rc_server == 0) {
			map_stmt_hash_to_info.erase(a->hash);
			num_stmt_with_ref_client_count_zero.fetch_sub(1, std::memory_order_relaxed);
			num_stmt_with_ref_server_count_zero.fetch_sub(1, std::memory_order_relaxed);
			to_remove.push_back(it->first);
		}
	}

	for (auto id : to_remove) {
		auto it = map_stmt_id_to_info.find(id);
		if (it != map_stmt_id_to_info.end()) {
			PgSQL_STMT_Global_info* a = it->second;
			//if (a->ref_count_client.load(std::memory_order_acquire) == 0 &&
			//	a->ref_count_server.load(std::memory_order_acquire) == 0)
			//{
				free_stmt_ids.push(id);
				map_stmt_id_to_info.erase(it);
				statuses.s_total -= a->ref_count_server.load(std::memory_order_relaxed);
				delete a;
			//}
		}
	}

	if (is_locked == false)
		unlock();
}

void PgSQL_STMT_Manager_v14::ref_count_client(uint64_t _stmt_id, int _v, bool lock) noexcept {
	if (lock) rdlock();

	if (auto s = map_stmt_id_to_info.find(_stmt_id); s != map_stmt_id_to_info.end()) {
		PgSQL_STMT_Global_info* stmt_info = s->second;
		ref_count_client(stmt_info, _v, false);
	}
	if (lock) unlock();
}

void PgSQL_STMT_Manager_v14::ref_count_client(PgSQL_STMT_Global_info* stmt_info, int _v, bool lock) noexcept {
	if (lock) rdlock();

	statuses.c_total += _v;
	
	if (_v == 1) {
		// increment: relaxed is fine for performance
		int prev = stmt_info->ref_count_client.fetch_add(1, std::memory_order_relaxed);
		// if prev was 0 -> we transitioned 0 -> 1: one fewer zero-count entry
		if (prev == 0) {
			num_stmt_with_ref_client_count_zero.fetch_sub(1, std::memory_order_relaxed);
		}
	} else if (_v == -1) {
		// decrement: use acq_rel to synchronize-with potential deleter
		int prev = stmt_info->ref_count_client.fetch_sub(1, std::memory_order_acq_rel);
		// prev is the value before subtraction
		if (prev == 1) {
			// we just transitioned to zero
			num_stmt_with_ref_client_count_zero.fetch_add(1, std::memory_order_relaxed);
		}
	} else {
		// support other increments/decrements (if needed)
		int prev = stmt_info->ref_count_client.fetch_add(_v,
			(_v > 0) ? std::memory_order_relaxed : std::memory_order_acq_rel);
		if (_v > 0 && prev == 0) num_stmt_with_ref_client_count_zero.fetch_sub(1, std::memory_order_relaxed);
		if (_v < 0 && prev + _v == 0) num_stmt_with_ref_client_count_zero.fetch_add(1, std::memory_order_relaxed);
	}
	
	if (lock) unlock();

	purge_stmts_if_needed(!lock);
}

void PgSQL_STMT_Manager_v14::ref_count_server(uint64_t _stmt_id, int _v, bool lock) noexcept {
	if (lock) rdlock();
	auto s = map_stmt_id_to_info.find(_stmt_id);
	if (s != map_stmt_id_to_info.end()) {
		statuses.s_total += _v;
		PgSQL_STMT_Global_info* stmt_info = s->second;

		if (_v == 1) {
			int prev = stmt_info->ref_count_server.fetch_add(1, std::memory_order_relaxed);
			if (prev == 0) {
				num_stmt_with_ref_server_count_zero.fetch_sub(1, std::memory_order_relaxed);
			}
		} else if (_v == -1) {
			int prev = stmt_info->ref_count_server.fetch_sub(1, std::memory_order_acq_rel);
			if (prev == 1) {
				num_stmt_with_ref_server_count_zero.fetch_add(1, std::memory_order_relaxed);
			}
		} else {
			int prev = stmt_info->ref_count_server.fetch_add(_v,
				(_v > 0) ? std::memory_order_relaxed : std::memory_order_acq_rel);
			if (_v > 0 && prev == 0) num_stmt_with_ref_server_count_zero.fetch_sub(1, std::memory_order_relaxed);
			if (_v < 0 && prev + _v == 0) num_stmt_with_ref_server_count_zero.fetch_add(1, std::memory_order_relaxed);
		}
	}
	if (lock) unlock();
}

PgSQL_STMTs_local_v14::~PgSQL_STMTs_local_v14() {
	// Note: we do not free the prepared statements because we assume that
	// if we call this destructor the connection is being destroyed anyway

	GloPgStmt->rdlock();
	if (is_client_) {
		for (auto it = stmt_name_to_global_ids.begin();
			it != stmt_name_to_global_ids.end(); ++it) {
			uint64_t global_stmt_id = it->second;
			GloPgStmt->ref_count_client(global_stmt_id, -1, false);
		}
	} else {
		for (auto it = backend_stmt_to_global_ids.begin();
			it != backend_stmt_to_global_ids.end(); ++it) {
			uint64_t global_stmt_id = it->second;
			GloPgStmt->ref_count_server(global_stmt_id, -1, false);
		}
	}
	GloPgStmt->unlock();
}


PgSQL_STMT_Global_info *PgSQL_STMT_Manager_v14::find_prepared_statement_by_hash(uint64_t hash, bool lock) {
	PgSQL_STMT_Global_info *ret = nullptr;  // assume we do not find it
	if (lock) {
		rdlock();
	}
	
	if (auto s = map_stmt_hash_to_info.find(hash); s != map_stmt_hash_to_info.end()) {
		ret = s->second;
		ref_count_client(ret, 1, false); // touch refcount for LRU purposes
	}

	if (lock) {
		unlock();
	}
	return ret;
}

PgSQL_STMT_Global_info* PgSQL_STMT_Manager_v14::find_prepared_statement_by_stmt_id(
    uint64_t id, bool lock) {
	PgSQL_STMT_Global_info*ret = nullptr;  // assume we do not find it
	if (lock) {
		rdlock();
	}

	if (auto s = map_stmt_id_to_info.find(id); s != map_stmt_id_to_info.end()) {
		ret = s->second;
	}

	if (lock) {
		unlock();
	}
	return ret;
}

uint32_t PgSQL_STMTs_local_v14::generate_new_backend_stmt_id() {
	assert(is_client_ == false);
	if (free_backend_ids.empty() == false) {
		uint32_t backend_stmt_id = free_backend_ids.top();
		free_backend_ids.pop();
		return backend_stmt_id;
	}
	local_max_stmt_id++;
	return local_max_stmt_id;
}

uint64_t PgSQL_STMTs_local_v14::find_global_id_from_stmt_name(std::string_view client_stmt_name) {
	uint64_t ret=0;
	uint32_t hash = SpookyHash::Hash32(client_stmt_name.data(), client_stmt_name.size(), 0);
	if (auto s = stmt_name_to_global_ids.find(hash); s != stmt_name_to_global_ids.end()) {
		ret = s->second;
	}
	return ret;
}

uint32_t PgSQL_STMTs_local_v14::find_backend_stmt_id_from_global_id(uint64_t global_id) {
	if (auto s = global_stmt_to_backend_ids.find(global_id); s != global_stmt_to_backend_ids.end()) {
		return s->second;
	}
	return 0;  // not found
}

bool PgSQL_STMTs_local_v14::client_close(const std::string& stmt_name) {
	uint32_t hash = SpookyHash::Hash32(stmt_name.data(), stmt_name.size(), 0);
	if (auto s = stmt_name_to_global_ids.find(hash); s != stmt_name_to_global_ids.end()) {  // found
		uint64_t global_stmt_id = s->second;
		stmt_name_to_global_ids.erase(s);
		GloPgStmt->ref_count_client(global_stmt_id, -1);
		std::pair<std::multimap<uint64_t,std::string>::iterator, std::multimap<uint64_t,std::string>::iterator> ret;
		ret = global_id_to_stmt_names.equal_range(global_stmt_id);
		for (std::multimap<uint64_t, std::string>::iterator it=ret.first; it!=ret.second; ++it) {
			if (it->second == stmt_name) {
				global_id_to_stmt_names.erase(it);
				break;
			}
		}
		return true;
	}
	return false;  // we don't really remove the prepared statement
}

void PgSQL_STMTs_local_v14::client_close_all() {
	GloPgStmt->rdlock();
	for (auto [_, global_stmt_id] : stmt_name_to_global_ids) {
		GloPgStmt->ref_count_client(global_stmt_id, -1, false);
	}
	GloPgStmt->unlock();
	stmt_name_to_global_ids.clear();
	global_id_to_stmt_names.clear();
}

PgSQL_STMT_Global_info* PgSQL_STMT_Manager_v14::add_prepared_statement(
    char *u, char *d, char *q, unsigned int ql,
    char *fc, Parse_Param_Types&& ppt, bool lock) {
	PgSQL_STMT_Global_info *ret = nullptr;
	uint64_t hash = stmt_compute_hash(
		u, d, q, ql, ppt);  // this identifies the prepared statement
	if (lock) {
		wrlock();
	}
	// try to find the statement
	if (auto f = map_stmt_hash_to_info.find(hash); f != map_stmt_hash_to_info.end()) {
		// found it!
		ret = f->second;
	} else {
		uint64_t next_id = 0;
		if (!free_stmt_ids.empty()) {
			next_id = free_stmt_ids.top();
			free_stmt_ids.pop();
		} else {
			next_id = next_statement_id;
			next_statement_id++;
		}

		auto stmt_info = std::make_unique<PgSQL_STMT_Global_info>(next_id, u, d, q, ql, fc, std::move(ppt), hash);
		// insert it in both maps
		map_stmt_id_to_info.insert(std::make_pair(stmt_info->statement_id, stmt_info.get()));
		map_stmt_hash_to_info.insert(std::make_pair(stmt_info->hash, stmt_info.get()));
		ret = stmt_info.release();
		num_stmt_with_ref_client_count_zero.fetch_add(1, std::memory_order_relaxed);
		num_stmt_with_ref_server_count_zero.fetch_add(1, std::memory_order_relaxed);
	}
	// Server refcount increment logic (atomic)
	int prev = ret->ref_count_server.fetch_add(1, std::memory_order_relaxed);
	if (prev == 0) {
		// Transition 0 -> 1: one fewer stmt with zero server refcount
		num_stmt_with_ref_server_count_zero.fetch_sub(1, std::memory_order_relaxed);
	}

	statuses.s_total++;
	if (lock) {
		unlock();
	}
	return ret;
}


void PgSQL_STMT_Manager_v14::get_memory_usage(uint64_t& prep_stmt_metadata_mem_usage, uint64_t& prep_stmt_backend_mem_usage) {
	prep_stmt_backend_mem_usage = 0;
	prep_stmt_metadata_mem_usage = sizeof(PgSQL_STMT_Manager_v14);
	rdlock();	
	prep_stmt_metadata_mem_usage += map_stmt_id_to_info.size() * (sizeof(uint64_t) + sizeof(PgSQL_STMT_Global_info*));
	prep_stmt_metadata_mem_usage += map_stmt_hash_to_info.size() * (sizeof(uint64_t) + sizeof(PgSQL_STMT_Global_info*));
	prep_stmt_metadata_mem_usage += free_stmt_ids.size() * (sizeof(uint64_t));
	for (const auto&[key, value] : map_stmt_id_to_info) {
		const PgSQL_STMT_Global_info* stmt_global_info = value;
		prep_stmt_metadata_mem_usage += stmt_global_info->total_mem_usage;
		prep_stmt_metadata_mem_usage += stmt_global_info->ref_count_server * 16; // ~16 bytes of memory utilized by global_stmt_id and stmt_id mappings
		prep_stmt_metadata_mem_usage += stmt_global_info->ref_count_client * 40; // ~40 bytes of memory utilized by client_stmt_name and global_stmt_id mappings;

		// backend
		prep_stmt_backend_mem_usage += stmt_global_info->ref_count_server; // FIXME: add backend memory usage
	}
	unlock();
}

void PgSQL_STMT_Manager_v14::get_metrics(uint64_t *c_unique, uint64_t *c_total,
                             uint64_t *stmt_max_stmt_id, uint64_t *cached,
                             uint64_t *s_unique, uint64_t *s_total) {
#ifdef DEBUG
	uint64_t c_u = 0;
	uint64_t c_t = 0;
	uint64_t m = 0;
	uint64_t c = 0;
	uint64_t s_u = 0;
	uint64_t s_t = 0;
#endif
	rdlock();
	statuses.cached = map_stmt_id_to_info.size();
	statuses.c_unique = statuses.cached - num_stmt_with_ref_client_count_zero;
	statuses.s_unique = statuses.cached - num_stmt_with_ref_server_count_zero;
#ifdef DEBUG
	for (std::map<uint64_t, PgSQL_STMT_Global_info *>::iterator it = map_stmt_id_to_info.begin();
	     it != map_stmt_id_to_info.end(); ++it) {
		const PgSQL_STMT_Global_info *a = it->second;
		c++;
		if (a->ref_count_client) {
			c_u++;
			c_t += a->ref_count_client;
		}
		if (a->ref_count_server) {
			s_u++;
			s_t += a->ref_count_server;
		}
		if (it->first > m) {
			m = it->first;
		}
	}
	assert (c_u == statuses.c_unique);
	assert (c_t == statuses.c_total);
	assert (c == statuses.cached);
	assert (s_t == statuses.s_total);
	assert (s_u == statuses.s_unique);
	*stmt_max_stmt_id = m;
#endif
	*stmt_max_stmt_id = next_statement_id; // this is max stmt_id, no matter if in used or not
	*c_unique = statuses.c_unique;
	*c_total = statuses.c_total;
	*cached = statuses.cached;
	*s_total = statuses.s_total;
	*s_unique = statuses.s_unique;
	unlock();
}


class PgSQL_PS_global_stats {
	public:
	uint64_t statement_id;
	char *username;
	char *dbname;
	uint64_t digest;
	unsigned long long ref_count_client;
	unsigned long long ref_count_server;
	char *query;
	int num_param_types;
	PgSQL_PS_global_stats(uint64_t stmt_id, const char *d, const char *u, uint64_t dig, const char *q,
		unsigned long long ref_c, unsigned long long ref_s, int params) {
		statement_id = stmt_id;
		digest = dig;
		query = strndup(q, pgsql_thread___query_digests_max_digest_length);
		username = strdup(u);
		dbname = strdup(d);
		ref_count_client = ref_c;
		ref_count_server = ref_s;
		num_param_types = params;
	}
	~PgSQL_PS_global_stats() {
		if (query) 
			free(query);
		if (username)
			free(username);
		if (dbname)
			free(dbname);
	}
	char **get_row() {
		char buf[128];
		char **pta=(char **)malloc(sizeof(char *)*PS_GLOBAL_STATUS_FIELD_NUM);
		snprintf(buf,sizeof(buf),"%lu",statement_id);
		pta[0]=strdup(buf);
		assert(dbname);
		pta[1]=strdup(dbname);
		assert(username);
		pta[2]=strdup(username);
		snprintf(buf,sizeof(buf),"0x%016llX", (long long unsigned int)digest);
		pta[3]=strdup(buf);
		assert(query);
		pta[4]=strdup(query);
		snprintf(buf,sizeof(buf),"%llu",ref_count_client);
		pta[5]=strdup(buf);
		snprintf(buf,sizeof(buf),"%llu",ref_count_server);
		pta[6]=strdup(buf);
		snprintf(buf,sizeof(buf),"%d",num_param_types);
		pta[7]=strdup(buf);

		return pta;
	}
	void free_row(char **pta) {
		int i;
		for (i=0;i<PS_GLOBAL_STATUS_FIELD_NUM;i++) {
			assert(pta[i]);
			free(pta[i]);
		}
		free(pta);
	}
};


SQLite3_result* PgSQL_STMT_Manager_v14::get_prepared_statements_global_infos() {
	proxy_debug(PROXY_DEBUG_MYSQL_QUERY_PROCESSOR, 4, "Dumping current prepared statements global info\n");
	auto result = std::make_unique<SQLite3_result>(PS_GLOBAL_STATUS_FIELD_NUM);
	result->add_column_definition(SQLITE_TEXT,"stmt_id");
	result->add_column_definition(SQLITE_TEXT,"database");
	result->add_column_definition(SQLITE_TEXT,"username");
	result->add_column_definition(SQLITE_TEXT,"digest");
	result->add_column_definition(SQLITE_TEXT,"query");
	result->add_column_definition(SQLITE_TEXT,"ref_count_client");
	result->add_column_definition(SQLITE_TEXT,"ref_count_server");
	result->add_column_definition(SQLITE_TEXT,"num_param_types");

	rdlock();
	for (auto it = map_stmt_id_to_info.begin(); it != map_stmt_id_to_info.end(); ++it) {
		PgSQL_STMT_Global_info *stmt_global_info = it->second;

		auto pgs = std::make_unique<PgSQL_PS_global_stats>(stmt_global_info->statement_id,
			stmt_global_info->dbname, stmt_global_info->username, stmt_global_info->hash, stmt_global_info->query,
			stmt_global_info->ref_count_client, stmt_global_info->ref_count_server, stmt_global_info->parse_param_types.size());
		char **pta = pgs->get_row();
		result->add_row(pta);
		pgs->free_row(pta);
	}
	unlock();
	return result.release();
}
