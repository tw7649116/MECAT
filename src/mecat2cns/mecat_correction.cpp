#include "mecat_correction.h"

#include "MECAT_AlnGraphBoost.H"

using namespace ns_banded_sw;

namespace ns_meap_cns {

#define FMAT 1
#define FDEL 2
#define FINS 4
#define UNDS 8

inline uint1
identify_one_consensus_item(CnsTableItem& cns_item, const int min_cov)
{
	uint1 ident = 0;
	int cov = cns_item.mat_cnt + cns_item.ins_cnt;
	if (cns_item.mat_cnt >= cov * 0.8) ident |= FMAT;
	if (cns_item.ins_cnt >= cov * 0.8) ident |= FINS;
	if (!ident) ident |= UNDS;
	if (cns_item.del_cnt >= cov * 0.4) ident |= FDEL;
	return ident;
}

struct CompareOverlapByOverlapSize
{
	bool operator()(const Overlap& a ,const Overlap& b)
	{
		const index_t ovlp_a = std::max(a.qend - a.qoff, a.send - a.soff);
		const index_t ovlp_b = std::max(b.qend - b.qoff, b.send - b.soff);
		return ovlp_a > ovlp_b;
	}
};

void
meap_add_one_aln(const std::string& qaln, const std::string& saln, index_t start_soff, CnsTableItem* cns_table, const char* org_seq)
{
	r_assert(qaln.size() == saln.size());
	const index_t aln_size = qaln.size();
	index_t i = 0;
	const char kGap = '-';
	while (i < aln_size)
	{
		const char q = qaln[i];
		const char s = saln[i];
		if (q == kGap && s == kGap) { ++i; continue; }
		
		if (q == s) { ++cns_table[start_soff].mat_cnt; cns_table[start_soff].base = s; ++start_soff; ++i; }
		else if (q == kGap) { ++cns_table[start_soff].ins_cnt; ++start_soff; ++i; }
		else
		{
			r_assert(s == kGap);
			index_t j = i + 1;
			while (j < aln_size && saln[j] == kGap) ++j;
			++cns_table[start_soff - 1].del_cnt;
			i = j;
		}
	}
}

void
meap_cns_one_indel(const int sb, const int se, CnsAlns& cns_vec, 
				   const int min_cov, std::string& aux_qstr,
				   std::string& aux_tstr, std::string& cns)
{
	AlnGraphBoost ag(se - sb + 1);
	int sb_out;
	for (CnsAln* iter = cns_vec.begin(); iter != cns_vec.end(); ++iter)
	{
		if ((*iter).retrieve_aln_subseqs(sb, se, aux_qstr, aux_tstr, sb_out))
		{
			ag.addAln(aux_qstr, aux_tstr, sb_out - sb + 1);
		}
	}		
	ag.mergeNodes();
	ag.consensus(min_cov * 0.4, cns);
}

void
meap_consensus_one_segment(CnsTableItem* cns_list, const int cns_list_size, 
						   uint1* cns_id_vec,
						   int start_soff, CnsAlns& cns_vec, 
						   std::string& aux_qstr, std::string& aux_tstr,
						   std::string& target, const int min_cov)
{
	for (int i = 0; i < cns_list_size; ++i) cns_id_vec[i] = identify_one_consensus_item(cns_list[i], min_cov);
	int i = 0, j; 
	std::string cns;
	target.clear();
	while (i < cns_list_size && !(cns_id_vec[i] & FMAT)) ++i;
	while (i < cns_list_size)
	{
		target.push_back(cns_list[i].base);
		j = i + 1;
		while (j < cns_list_size && !(cns_id_vec[j] & FMAT)) ++j;
		
		bool need_refinement = false;
		for (int k = i; k < j; ++k)
			if ((cns_id_vec[k] & UNDS) || (cns_id_vec[k] & FDEL)) { need_refinement = true; break; }
		if (need_refinement)
		{
			meap_cns_one_indel(i + start_soff, j + start_soff, cns_vec, cns_list[i].mat_cnt + cns_list[i].ins_cnt, aux_qstr, aux_tstr, cns);
			if (cns.size() > 2) target.append(cns.data() + 1, cns.size() - 2);
		}
		i = j;
	}
}

struct CmpMappingRangeBySoff
{
	bool operator()(const MappingRange& a, const MappingRange& b)
	{
		return (a.start == b.start) ? (a.end > b.end) : (a.start < b.start);
	}
};

void 
get_effective_ranges(std::vector<MappingRange>& mranges, std::vector<MappingRange>& eranges, const int read_size, const int min_size)
{
	eranges.clear();
	if (mranges.size() == 0) return;
	std::vector<MappingRange>::iterator iter;
	for (iter = mranges.begin(); iter != mranges.end(); ++iter)
		if (iter->start <= 500 && read_size - iter->end <= 500)
		{
			eranges.push_back(MappingRange(0, read_size));
			return;
		}

	std::sort(mranges.begin(), mranges.end(), CmpMappingRangeBySoff());
	const int nr = mranges.size();
	int i = 0, j;
	int left = mranges[i].start, right;
	while (i < nr)
	{
		j = i + 1;
		while (j < nr && mranges[j].end <= mranges[i].end) ++j;
		if (j == nr)
		{
			right = mranges[i].end;
			if (right - left >= min_size * 0.95) eranges.push_back(MappingRange(left, right));
			break;
		}
		if (mranges[i].end - mranges[j].start < 1000)
		{
			right = std::min(mranges[i].end, mranges[j].start);
			if (right - left >= min_size * 0.95) eranges.push_back(MappingRange(left, right));
			left = std::max(mranges[i].end, mranges[j].start);
		}
		i = j;
	}
}

void
meap_consensus_one_read_m4(ConsensusThreadData* ctd, const index_t read_id, const index_t sid, const index_t eid)
{
	PackedDB& reads = *ctd->reads;
	ExtensionCandidate* overlaps = ctd->candidates;
	DiffRunningData* drd_s = ctd->drd_s;
	DiffRunningData* drd = NULL;
	M5Record* m5 = ctd->m5;
	CnsAlns& cns_vec = ctd->cns_alns;
	std::vector<CnsResult>& cns_results = ctd->cns_results;
	const index_t read_size = overlaps[sid].ssize;
	std::vector<char>& qstr = ctd->query;
	std::vector<char>& tstr = ctd->target;
	tstr.resize(read_size);
	reads.GetSequence(read_id, true, tstr.data(), read_size);
	std::string& nqstr = ctd->qaln;
	std::string& ntstr = ctd->saln;

	index_t L, R;
	if (eid - sid <= MAX_CNS_OVLPS)
	{
		L = sid;
		R = eid;
	}
	else
	{
		L = sid;
		R = L + MAX_CNS_OVLPS;    
		std::sort(overlaps + sid, overlaps + eid, CompareOverlapByOverlapSize());
	}

	CnsTableItem* cns_table = ctd->cns_table;
	std::for_each(cns_table, cns_table + read_size, CnsTableItemCleaner());
	cns_vec.clear();
	for (index_t i = L; i < R; ++i)
	{
		Overlap& ovlp = overlaps[i];
		qstr.resize(ovlp.qsize);
		reads.GetSequence(ovlp.qid, ovlp.qdir == FWD, qstr.data(), ovlp.qsize);
		index_t qext = ovlp.qext;
		index_t sext = ovlp.sext;
		if (ovlp.qdir == REV) qext = ovlp.qsize - 1 - qext;
		drd = drd_s;
		bool r = GetAlignment(qstr.data(), qext, qstr.size(), tstr.data(), sext, tstr.size(), drd, *m5);
		if (r)
		{
			normalize_gaps(m5qaln(*m5), m5saln(*m5), strlen(m5qaln(*m5)), nqstr, ntstr, true);
			meap_add_one_aln(nqstr, ntstr, m5soff(*m5), cns_table, tstr.data());
			cns_vec.add_aln(m5soff(*m5), m5send(*m5), nqstr, ntstr);
		}
	}
	
	std::vector<MappingRange> mranges, eranges;
	cns_vec.get_mapping_ranges(mranges);
	get_effective_ranges(mranges, eranges, read_size, ctd->rco.min_size);
	std::vector<MappingRange>::iterator miter;
	
	index_t beg = 0, end;
	const int min_cov = ctd->rco.min_cov;
	const int min_size = ctd->rco.min_size;
	CnsResult cns_result;
	cns_result.id = read_id;
	for (miter = eranges.begin(); miter != eranges.end(); ++miter)
	{
		int L = miter->start, R = miter->end;
		beg = L;
		while (beg < R)
		{
			while (beg < R && cns_table[beg].mat_cnt + cns_table[beg].ins_cnt < min_cov) ++beg;
			end = beg + 1;
			while (end < R && cns_table[end].mat_cnt + cns_table[end].ins_cnt >= min_cov) ++end;
			if (end - beg >= 0.95 * min_size)
			{
				meap_consensus_one_segment(cns_table + beg, end - beg, ctd->id_list,
										   beg, cns_vec, nqstr, ntstr, cns_result.seq, min_cov);
				cns_result.range[0] = beg;
				cns_result.range[1] = end;
				cns_results.push_back(cns_result);
			}
			
			beg = end;
		}
	}
}

inline int
estimate_ovlp_size(const ExtensionCandidate& ec)
{
	r_assert(ec.sdir == FWD);
	int qext = ec.qext;
	if (ec.qdir == REV) qext = ec.qsize - 1 - ec.qext;
	int num1 = std::min(qext, ec.sext);
	int num2 = std::min(ec.qsize - qext, ec.ssize - ec.sext);
	return num1 + num2;
}

struct CmpExtensionCandidateByScore
{
	bool operator()(const ExtensionCandidate& a, const ExtensionCandidate& b)
	{
		//int ova = estimate_ovlp_size(a);
		//int ovb = estimate_ovlp_size(b);
		//return ova > ovb;
        return a.score > b.score;
	}
};

inline bool
check_ovlp_mapping_range(const int qb, const int qe, const int qs,
						 const int sb, const int se, const int ss)
{
	const double ratio = 0.88;
	const int oq = qe - qb;
	const int qqs = qs * ratio;
	const int os = se - sb;
	const int qss = ss * ratio;
	return oq >= qqs || os >= qss;
}

inline bool
check_cov_stats(u1_t* cov_stats, int soff, int send)
{
	const int max_cov = 20;
	int n = 0;
	for (int i = soff; i < send; ++i)
		if (cov_stats[i] >= max_cov)
			++n;
	if (send - soff >= n + 200) 
	{
		for (int i = soff; i < send; ++i) ++cov_stats[i];
		return true;
	}
	return false;
}

void
meap_consensus_one_read_can(ConsensusThreadData* ctd, const index_t read_id, const index_t sid, const index_t eid)
{
	PackedDB& reads = *ctd->reads;
	ExtensionCandidate* candidates = ctd->candidates;
	DiffRunningData* drd_s = ctd->drd_s;
	DiffRunningData* drd = NULL;
	M5Record* m5 = ctd->m5;
	CnsAlns& cns_vec = ctd->cns_alns;
	std::vector<CnsResult>& cns_results = ctd->cns_results;
	const index_t read_size = candidates[sid].ssize;
	std::vector<char>& qstr = ctd->query;
	std::vector<char>& tstr = ctd->target;
	tstr.resize(read_size);
	reads.GetSequence(read_id, true, tstr.data(), read_size);
	std::string& nqstr = ctd->qaln;
	std::string& ntstr = ctd->saln;
	
	std::sort(candidates + sid, candidates + eid, CmpExtensionCandidateByScore());
	int num_added = 0;
    int num_ext = 0;
    const int max_ext = 200;
	CnsTableItem* cns_table = ctd->cns_table;
	std::for_each(cns_table, cns_table + read_size, CnsTableItemCleaner());
	cns_vec.clear();
	std::set<int> used_ids;
	u1_t* cov_stats = ctd->id_list;
	std::fill(cov_stats, cov_stats + MAX_SEQ_SIZE, 0);
	for (idx_t i = sid; i < eid && num_added < MAX_CNS_OVLPS && num_ext < max_ext; ++i)
	{
        ++num_ext;
		ExtensionCandidate& ec = candidates[i];
		r_assert(ec.sdir == FWD);
		if (used_ids.find(ec.qid) != used_ids.end()) continue;
		qstr.resize(ec.qsize);
		reads.GetSequence(ec.qid, ec.qdir == FWD, qstr.data(), ec.qsize);
		index_t qext = ec.qext;
		index_t sext = ec.sext;
		if (ec.qdir == REV) qext = ec.qsize - 1 - qext;
		drd = drd_s;
		bool r = GetAlignment(qstr.data(), qext, qstr.size(), tstr.data(), sext, tstr.size(), drd, *m5);
		if (r && check_ovlp_mapping_range(m5qoff(*m5), m5qend(*m5), ec.qsize, m5soff(*m5), m5send(*m5), ec.ssize))
		{
			if (check_cov_stats(cov_stats, m5soff(*m5), m5send(*m5)))
			{
				++num_added;
				used_ids.insert(ec.qid);
				normalize_gaps(m5qaln(*m5), m5saln(*m5), strlen(m5qaln(*m5)), nqstr, ntstr, true);
				meap_add_one_aln(nqstr, ntstr, m5soff(*m5), cns_table, tstr.data());
				cns_vec.add_aln(m5soff(*m5), m5send(*m5), nqstr, ntstr);
			}
		}
	}
	
	std::vector<MappingRange> mranges, eranges;
	cns_vec.get_mapping_ranges(mranges);
	get_effective_ranges(mranges, eranges, read_size, ctd->rco.min_size);
	std::vector<MappingRange>::iterator miter;
	
	index_t beg = 0, end;
	const int min_cov = ctd->rco.min_cov;
	const int min_size = ctd->rco.min_size;
	CnsResult cns_result;
	cns_result.id = read_id;
	for (miter = eranges.begin(); miter != eranges.end(); ++miter)
	{
		int L = miter->start, R = miter->end;
		beg = L;
		while (beg < R)
		{
			while (beg < R && cns_table[beg].mat_cnt + cns_table[beg].ins_cnt < min_cov) ++beg;
			end = beg + 1;
			while (end < R && cns_table[end].mat_cnt + cns_table[end].ins_cnt >= min_cov) ++end;
			if (end - beg >= 0.95 * min_size)
			{
				meap_consensus_one_segment(cns_table + beg, end - beg, ctd->id_list,
										   beg, cns_vec, nqstr, ntstr, cns_result.seq, min_cov);
				cns_result.range[0] = beg;
				cns_result.range[1] = end;
				cns_results.push_back(cns_result);
			}
			
			beg = end;
		}
	}
}

} // namespace ns_meap_cns {
