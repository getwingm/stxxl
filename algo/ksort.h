#ifndef KSORT_HEADER
#define KSORT_HEADER

/***************************************************************************
 *            ksort.h
 *
 *  Fri Oct  4 19:18:04 2002
 *  Copyright  2002  Roman Dementiev
 *  dementiev@mpi-sb.mpg.de
 ****************************************************************************/

#include <list>

#include "../mng/mng.h"
#include "../common/rand.h"
#include "../mng/adaptor.h"
#include "../common/simple_vector.h"
#include "../common/switch.h"
#include "interleaved_alloc.h"
#include "intksort.h"
#include "adaptor.h"
#include "async_schedule.h"
#include "../mng/block_prefetcher.h"
#include "../mng/buf_writer.h"
#include "run_cursor.h"
#include "loosertree.h"
#include "inmemsort.h"

//#define SORT_OPT_PREFETCHING
//#define INTERLEAVED_ALLOC

#define OPT_MERGING

__STXXL_BEGIN_NAMESPACE

//! \weakgroup stllayer STL-user layer
//! Layer which groups STL compatible algorithms and containters

//! \weakgroup stlalgo Algorithms
//! \ingroup stllayer
//! Algorithms with STL-compatible interface
//! \{

template <typename _BIDTp,typename _KeyTp>
struct trigger_entry
{
	typedef _BIDTp bid_type;
	typedef _KeyTp key_type;

	bid_type bid;
	key_type key;
	
	operator bid_type()
	{
		return bid;
	};
};



template <typename _BIDTp,typename _KeyTp>
inline bool operator < (const trigger_entry<_BIDTp,_KeyTp> & a, 
												const trigger_entry<_BIDTp,_KeyTp> & b)
{
	return (a.key < b.key);
};

template <typename _BIDTp,typename _KeyTp>
inline bool operator > (const trigger_entry<_BIDTp,_KeyTp> & a,
												const trigger_entry<_BIDTp,_KeyTp> & b)
{
	return (a.key > b.key);
};

template <typename type>
struct type_key
{
	typedef typename type::key_type key_type;
	key_type key;
	type * ptr;
	
	type_key() {};
	type_key(key_type k, type * p):key (k), ptr (p)
	{
	};
};

template <typename type>
bool operator  < (const type_key<type> & a, const type_key<type> & b)
{
		return a.key < b.key;
}

template <typename type>
bool operator  > (const type_key<type> & a, const type_key<type> & b)
{
		return a.key > b.key;
}



template <typename block_type,typename bid_type>
struct write_completion_handler
{
	block_type * block;
	bid_type bid;
	request_ptr * req;
	void operator () (request * completed_req)
	{
		*req = block->read(bid);
	}
};

template <typename type_key_,
					typename block_type,
					typename run_type,
					typename input_bid_iterator,
					typename key_extractor>
inline void write_out(
					type_key_ *begin,
					type_key_ * end,
					block_type *& cur_blk,
					const block_type * end_blk,
					int & out_block,
					int & out_pos,
					run_type & run,
					write_completion_handler<block_type,typename block_type::bid_type> *& next_read,
					request_ptr * write_reqs,
					request_ptr * read_reqs,
					input_bid_iterator & it,
					key_extractor keyobj)
{
	typedef typename block_type::bid_type bid_type;
	typedef typename block_type::type type;
	
	block_manager *bm = block_manager::get_instance ();
	type * elem = cur_blk->elem;
	for (type_key_ * p = begin; p < end; p++)
	{
		elem[out_pos++] = *(p->ptr);
		
		if (out_pos >= block_type::size)
		{
			run[out_block].key = keyobj(*(cur_blk->elem));
			
			if (cur_blk < end_blk)
			{
					next_read->block = cur_blk;
					next_read->req = read_reqs + out_block;
					read_reqs[out_block] = NULL;
					bm->delete_block( next_read->bid = *(it++) );
																																
					write_reqs[out_block] = cur_blk->write (	
							run[out_block].bid,
							                  // postpone read of block from next run
							*(next_read++));  // after write of block from this run
					
			}
			else
			{
				write_reqs[out_block] = cur_blk->write (run[out_block].bid);
			}
										
			cur_blk++;
			elem = cur_blk->elem;
			out_block++;
			out_pos = 0;
		}
	}
}

template <
					typename block_type,
					typename run_type,
					typename input_bid_iterator,
					typename key_extractor>
void
create_runs(
		input_bid_iterator it,
		run_type ** runs,
		const unsigned nruns,
		const unsigned m2,
		key_extractor keyobj)
{
	typedef typename block_type::value_type type;
	typedef typename block_type::bid_type bid_type;
	typedef type_key<type> type_key_;
	typedef typename type_key_::key_type key_type;
	
	block_manager *bm = block_manager::get_instance ();
	block_type *Blocks1 = new block_type[m2];
	block_type *Blocks2 = new block_type[m2];
	type_key_ *refs1 = new type_key_[m2 * Blocks1->size];
	type_key_ *refs2 = new type_key_[m2 * Blocks1->size];
	request_ptr * read_reqs = new request_ptr[m2];
	request_ptr * write_reqs = new request_ptr[m2];
	write_completion_handler<block_type,bid_type> * next_run_reads = 
		new write_completion_handler<block_type,bid_type>[m2];
	
	run_type *run;
	run = *runs;
	int run_size = (*runs)->size ();
	key_type offset = 0;
	const int log_k1 = static_cast<int>(ceil(log2(m2 * block_type::size * sizeof(type_key_)/STXXL_L2_SIZE)));
	const int log_k2 = int(log2(m2 * Blocks1->size)) - log_k1 - 1;
	STXXL_VERBOSE("log_k1: "<<log_k1<<" log_k2:"<<log_k2)
	const int k1 = 1 << log_k1;
	const int k2 = 1 << log_k2;
	int *bucket1 = new int[k1];
	int *bucket2 = new int[k2];
	int i;
	
	disk_queues::get_instance ()->set_priority_op (disk_queue::WRITE);
	
	for (i = 0; i < run_size; i++)
	{
		bid_type bid = *(it++);
		read_reqs[i] = Blocks1[i].read(bid);
		bm->delete_block(bid);
	}
	
	unsigned k = 0;
	const int shift1 = sizeof(typename block_type::value_type::key_type)*8 - log_k1;
	const int shift2 = shift1 - log_k2;

	for (; k < nruns; k++)
	{
		run = runs[k];
		run_size = run->size ();
		
		std::fill(bucket1,bucket1 + k1,0);

		type_key_ * ref_ptr = refs1;
		for (i = 0; i < run_size; i++)
		{
			if(k)
				write_reqs[i]->wait();
	
			read_reqs[i]->wait();

			classify_block(Blocks1[i].begin(),Blocks1[i].end(),ref_ptr,bucket1,offset,shift1,keyobj);
		}
				
		exclusive_prefix_sum(bucket1, k1);
		classify(refs1, refs1 + run_size * Blocks1->size, refs2, bucket1,
			  offset, shift1);
		
		int out_block = 0;
		int out_pos = 0;
		unsigned int next_run_size = (k < nruns - 1)?(runs[k + 1]->size ()):0;
		
		// recurse on each bucket
		type_key_ *c = refs2;
		type_key_ *d = refs1;
		block_type *cur_blk = Blocks2;
		block_type *end_blk = Blocks2 + next_run_size;
		write_completion_handler<block_type,bid_type> * next_read = next_run_reads;
		
		for (i = 0; i < k1; i++)
		{
			type_key_ *cEnd = refs2 + bucket1[i];
			type_key_ *dEnd = refs1 + bucket1[i];
			
			l1sort(c, cEnd, d, bucket2, k2,
				offset + (key_type(1)<<key_type(shift1)) * key_type(i) , shift2); // key_type,key_type,... paranoia 
			
			write_out(
							d,dEnd,cur_blk,end_blk,
							out_block,out_pos,*run,next_read,
							write_reqs,read_reqs,it,keyobj);
			
			c = cEnd;
			d = dEnd;
		}

		std::swap (Blocks1, Blocks2);
	}
	
	wait_all (write_reqs, m2);

	delete [] bucket1;
	delete [] bucket2;
	delete [] refs1;
	delete [] refs2;
	delete [] Blocks1;
	delete [] Blocks2;
	delete [] next_run_reads;
	delete [] read_reqs;
	delete [] write_reqs;
}

template <typename block_type,
					typename prefetcher_type,
					typename key_extractor>
struct run_cursor2_cmp
{
	typedef run_cursor2<block_type,prefetcher_type> cursor_type;
	key_extractor keyobj;
	run_cursor2_cmp(key_extractor keyobj_) { keyobj = keyobj_; }
	inline bool operator  () (const cursor_type & a, const cursor_type & b)
	{
		if (UNLIKELY (b.empty ()))
			return true;	// sentinel emulation
		if (UNLIKELY (a.empty ()))
			return false;	//sentinel emulation

		return (keyobj(a.current()) < keyobj(b.current ()));
	};
	private:
	run_cursor2_cmp() {};
};


//#include "loosertree.h"

template < typename block_type,typename run_type, typename key_extractor>
void merge_runs(run_type ** in_runs, unsigned nruns, run_type * out_run,unsigned  _m, key_extractor keyobj)
{
	typedef block_prefetcher<block_type,typename run_type::iterator> prefetcher_type;
	typedef run_cursor2<block_type,prefetcher_type> run_cursor_type;
	
	unsigned int i;
	run_type consume_seq(out_run->size());

	int * prefetch_seq = new int[out_run->size()];

	typename run_type::iterator copy_start = consume_seq.begin ();
	for (i = 0; i < nruns; i++)
	{
		// TODO: try to avoid copy
		copy_start = std::copy(
						in_runs[i]->begin (),
						in_runs[i]->end (),
						copy_start	);
	}
	std::stable_sort (consume_seq.begin (), consume_seq.end ());

	unsigned disks_number = config::get_instance()->disks_number ();
	
	#ifdef PLAY_WITH_OPT_PREF
	const int n_write_buffers = 4 * disks_number;
	#else
	const int n_prefetch_buffers = std::max( 2 * disks_number , (3 * (int(_m) - nruns) / 4));
        const int n_write_buffers = std::max( 2 * disks_number , int(_m) - nruns - n_prefetch_buffers );
	// heuristic
	const int n_opt_prefetch_buffers = 2 * disks_number + (3*(n_prefetch_buffers - 2 * disks_number))/10;
	#endif
	
	#ifdef SORT_OPT_PREFETCHING
	compute_prefetch_schedule(
			consume_seq,
			prefetch_seq,
			n_opt_prefetch_buffers,
			disks_number );
	#else
	for(i=0;i<out_run->size();i++)
		prefetch_seq[i] = i;
	#endif
	
	
	prefetcher_type prefetcher(	consume_seq.begin(),
															consume_seq.end(),
															prefetch_seq,
															nruns + n_prefetch_buffers);
	
	buffered_writer<block_type> writer(n_write_buffers,n_write_buffers/2);
	
	unsigned out_run_size = out_run->size();

	run_cursor2_cmp<block_type,prefetcher_type,key_extractor> cmp(keyobj);
	looser_tree<
							run_cursor_type,
							run_cursor2_cmp<block_type,prefetcher_type,key_extractor>,
							block_type::size> loosers (&prefetcher, nruns, cmp);


	block_type *out_buffer = writer.get_free_block();

	for (i = 0; i < out_run_size; i++)
	{
		loosers.multi_merge (out_buffer->elem);
		(*out_run)[i].key = keyobj(out_buffer->elem[0]);
		out_buffer = writer.write(out_buffer,(*out_run)[i].bid);
	}
	
	delete [] prefetch_seq;

	block_manager *bm = block_manager::get_instance ();
	for (i = 0; i < nruns; i++)
	{
		unsigned sz = in_runs[i]->size ();
		for (unsigned j = 0; j < sz; j++)
			bm->delete_block((*in_runs[i])[j].bid);
		delete in_runs[i];
	}
}


template <typename block_type,
					typename alloc_strategy,
					typename input_bid_iterator,
					typename key_extractor>

simple_vector< trigger_entry<typename block_type::bid_type,typename block_type::value_type::key_type> > * 
	ksort_blocks(input_bid_iterator input_bids,unsigned _n,unsigned _m,key_extractor keyobj)
{
	typedef typename block_type::value_type type;
	typedef typename block_type::bid_type bid_type;
	typedef trigger_entry< bid_type,typename type::key_type> trigger_entry_type;
	typedef simple_vector< trigger_entry_type > run_type;
	typedef typename interleaved_alloc_traits<alloc_strategy>::strategy interleaved_alloc_strategy;
	
	unsigned int m2 = div_and_round_up(_m,2);
  const unsigned int m2_rf = m2 * block_type::raw_size / 
    (block_type::raw_size + block_type::size*sizeof(type_key<type>));
  STXXL_VERBOSE("Reducing number of blocks in a run from "<< m2 << " to "<<
    m2_rf<<" due to key size: "<<sizeof(typename type::key_type)<<" bytes")
  m2 = m2_rf;
	unsigned int full_runs = _n / m2;
	unsigned int partial_runs = ((_n % m2) ? 1 : 0);
	unsigned int nruns = full_runs + partial_runs;
	unsigned int i;
	
	config *cfg = config::get_instance ();
	block_manager *mng = block_manager::get_instance ();
	int ndisks = cfg->disks_number ();
	
	STXXL_VERBOSE ("n=" << _n << " nruns=" << nruns << "=" << full_runs << "+" << partial_runs) 
	
#ifdef STXXL_IO_STATS
	stats *iostats = stats::get_instance ();
	iostats += 0;
	// iostats->reset ();
#endif
	
	double begin = stxxl_timestamp (), after_runs_creation, end;
  (void)(begin);

	run_type **runs = new run_type *[nruns];

	for (i = 0; i < full_runs; i++)
		runs[i] = new run_type (m2);

#ifdef INTERLEAVED_ALLOC
	if (partial_runs)
	{
		unsigned int last_run_size = _n - full_runs * m2;
		runs[i] = new run_type (last_run_size);

		mng->new_blocks (interleaved_alloc_strategy (nruns, 0, ndisks),
				 RunsToBIDArrayAdaptor2 < block_type::raw_size,run_type >
				 (runs, 0, nruns, last_run_size),
				 RunsToBIDArrayAdaptor2 < block_type::raw_size,run_type >
				 (runs, _n, nruns, last_run_size));

	}
	else
		mng->new_blocks (interleaved_alloc_strategy (nruns, 0, ndisks),
				 RunsToBIDArrayAdaptor < block_type::raw_size,run_type >
				 (runs, 0, nruns),
				 RunsToBIDArrayAdaptor < block_type::raw_size,run_type >
				 (runs, _n, nruns));
#else
	
		if (partial_runs)
			runs[i] = new run_type (_n - full_runs * m2);
		
		for(i=0;i<nruns;i++)
		{
			mng->new_blocks(	alloc_strategy(0,ndisks),
						trigger_entry_iterator<typename run_type::iterator,block_type::raw_size>(runs[i]->begin()),
						trigger_entry_iterator<typename run_type::iterator,block_type::raw_size>(runs[i]->end())	);
		}
#endif
	  
	create_runs< block_type,
							 run_type,
							 input_bid_iterator,
							 key_extractor> (input_bids, runs, nruns,m2,keyobj);

	after_runs_creation = stxxl_timestamp ();
		
#ifdef COUNT_WAIT_TIME
	double io_wait_after_rf = stxxl::wait_time_counter;
	io_wait_after_rf += 0.0; // to get rid of the 'unused variable warning'
#endif

	disk_queues::get_instance ()->set_priority_op (disk_queue::WRITE);
    
	// Optimal merging: merge r = pow(nruns,1/ceil(log(nruns)/log(m))) at once
		
	const int merge_factor = static_cast<int>(ceil(pow(nruns,1./ceil(log(nruns)/log(_m)))));
	run_type **new_runs;
	
	while(nruns > 1)
	{
		int new_nruns = div_and_round_up(nruns,merge_factor);
		STXXL_VERBOSE("Starting new merge phase: nruns: "<<nruns<<
			" opt_merge_factor: "<<merge_factor<<" m:"<<_m<<" new_nruns: "<<new_nruns)
		
		new_runs = new run_type *[new_nruns];
		
		int runs_left = nruns;
		int cur_out_run = 0;
		int blocks_in_new_run = 0;
		
		while(runs_left > 0)
		{
			int runs2merge = STXXL_MIN(runs_left,merge_factor);
			blocks_in_new_run = 0;
			for(unsigned i = nruns - runs_left; i < (nruns - runs_left + runs2merge);i++)
				blocks_in_new_run += runs[i]->size();
			// allocate run
			new_runs[cur_out_run++] = new run_type(blocks_in_new_run);
			runs_left -= runs2merge;
		}
		// allocate blocks in the new runs
		mng->new_blocks( interleaved_alloc_strategy(new_nruns, 0, ndisks),
						 RunsToBIDArrayAdaptor2<block_type::raw_size,run_type> (new_runs,0,new_nruns,blocks_in_new_run),
						 RunsToBIDArrayAdaptor2<block_type::raw_size,run_type> (new_runs,_n,new_nruns,blocks_in_new_run));
		// merge all
		runs_left = nruns;
		cur_out_run = 0;
		while(runs_left > 0)
		{
				int runs2merge = STXXL_MIN(runs_left,merge_factor);
				STXXL_VERBOSE("Merging "<<runs2merge<<" runs")
				merge_runs<block_type,run_type,key_extractor> (runs + nruns - runs_left, 
						runs2merge ,*(new_runs + (cur_out_run++)),_m,keyobj);
				runs_left -= runs2merge;
		}
		
		nruns = new_nruns;
		delete [] runs;
		runs = new_runs;
	}
	
	
	run_type * result = *runs;
	delete [] runs;
	
	end = stxxl_timestamp ();

	STXXL_VERBOSE ("Elapsed time        : " << end - begin << " s. Run creation time: " << 
	after_runs_creation - begin << " s")
#ifdef STXXL_IO_STATS
	STXXL_VERBOSE ("reads               : " << iostats->get_reads ()) 
	STXXL_VERBOSE ("reads(volume)       : " << iostats->get_read_volume ()<< " bytes") 
	STXXL_VERBOSE ("writes              : " << iostats->get_writes () << " bytes")
	STXXL_VERBOSE ("writes(volume)      : " << iostats->get_written_volume ())
	STXXL_VERBOSE ("read time           : " << iostats->get_read_time () << " s") 
	STXXL_VERBOSE ("write time          : " << iostats->get_write_time () <<" s")
	STXXL_VERBOSE ("parallel read time  : " << iostats->get_pread_time () << " s")
	STXXL_VERBOSE ("parallel write time : " << iostats->get_pwrite_time () << " s")
	STXXL_VERBOSE ("parallel io time    : " << iostats->get_pio_time () << " s")
#endif
#ifdef COUNT_WAIT_TIME
	STXXL_VERBOSE ("Time in I/O wait(rf): " << io_wait_after_rf << " s")
	STXXL_VERBOSE ("Time in I/O wait    : " << stxxl::wait_time_counter << " s")
#endif
	
	return result; 
}


template <typename record_type, typename key_extractor>
class key_comparison
{
  key_extractor ke;
public:
  key_comparison() {}
  key_comparison(key_extractor ke_): ke(ke_){}
  bool operator () (const record_type & a, const record_type & b)
  {
    return ke(a) < ke(b);
  }
};

/*! \page key_extractor Key extractor concept
 
  Model of \b Key \b extractor concept must:
   - define type \b key_type ,
   - provide \b operator() that returns key value of an object of user type
   - provide \b max_value method that returns a value that is \b greater than all  
   other objects of user type in respect to the key obtained by this key extractor ,
   - provide \b min_value method that returns a value that is \b less than all 
   other objects of user type in respect to the key obtained by this key extractor ,
   - operator > , operator <, operator == and operator != on type \b key_type must be defined.
 
  Example: extractor class \b GetWeight, that extracts weight from an \b Edge
  \verbatim
 
  struct Edge
  {
     unsigned src,dest,weight;
     Edge(unsigned s,unsigned d,unsigned w):src(s),dest(d),weight(w){}
  };
 
  struct GetWeight
  {
    typedef unsigned key_type;
    key_type operator() (const Edge & e)
    {
 		  return e.weight;
    }
    Edge min_value() const { return Edge(0,0,0); };
    Edge max_value() const { return Edge(0,0,0xffffffff); };
  };
  \endverbatim
 
 */



//! \brief External sorting routine for records with keys
//! \param first_ object of model of \c ext_random_access_iterator concept
//! \param last_ object of model of \c ext_random_access_iterator concept
//! \param keyobj \link key_extractor key extractor \endlink object
//! \param M__ amount of memory for internal use (in bytes)
//! \remark Implements external merge sort described in [Dementiev & Sanders'03]
//! \remark Order in the result is non-stable
template <typename ExtIterator_,typename KeyExtractor_>
void ksort(ExtIterator_ first_, ExtIterator_ last_,KeyExtractor_ keyobj,unsigned M__)
{
	typedef simple_vector< trigger_entry<typename ExtIterator_::bid_type,
      typename KeyExtractor_::key_type> > run_type;
	typedef typename ExtIterator_::vector_type::value_type value_type;
	typedef typename ExtIterator_::block_type block_type;
	
	assert(2*block_type::raw_size <= M__);
	
	unsigned n=0;
	block_manager *mng = block_manager::get_instance ();
	
	first_.flush();
	
	if((last_ - first_)*sizeof(value_type) < M__)
	{
		stl_in_memory_sort(first_,last_,key_comparison<value_type,KeyExtractor_>(keyobj));
	}
	else
	{
		if(first_.block_offset()) 
		{
			if(last_.block_offset()) // first and last element reside 
																// not in the beginning of the block
			{
				typename ExtIterator_::block_type * first_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::block_type * last_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::bid_type first_bid,last_bid;
				request_ptr req;
				
				req = first_block->read(*first_.bid());
				mng->new_blocks( FR(), &first_bid,(&first_bid) + 1); // try to overlap
				mng->new_blocks( FR(), &last_bid,(&last_bid) + 1);
				req->wait();
				
			
				req = last_block->read(*last_.bid());
				
				unsigned i=0;
				for(;i<first_.block_offset();i++)
				{
					first_block->elem[i] = keyobj.min_value();
				}
				
				req->wait();
				
				req = first_block->write(first_bid);
				for(i=last_.block_offset(); i < block_type::size;i++)
				{
					last_block->elem[i] = keyobj.max_value();
				}
				
				req->wait();
				
				req = last_block->write(last_bid);
				
				n=last_.bid() - first_.bid() + 1;
				
				std::swap(first_bid,*first_.bid());
				std::swap(last_bid,*last_.bid());
				
				req->wait();
				
				delete first_block;
				delete last_block;

				run_type * out =
						ksort_blocks<	
													typename ExtIterator_::block_type,
													typename ExtIterator_::vector_type::alloc_strategy,
													typename ExtIterator_::bids_container_iterator ,
													KeyExtractor_>
														 (first_.bid(),n,M__/block_type::raw_size,keyobj);
					
				
				first_block = new typename ExtIterator_::block_type;
				last_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::block_type * sorted_first_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::block_type * sorted_last_block = new typename ExtIterator_::block_type;
				request_ptr * reqs = new request_ptr [2];
				
				reqs[0] = first_block->read(first_bid);
				reqs[1] = sorted_first_block->read((*(out->begin())).bid);
				wait_all(reqs,2);
				
				reqs[0] = last_block->read(last_bid);
				reqs[1] = sorted_last_block->read( ((*out)[out->size() - 1]).bid);
				
				for(i=first_.block_offset();i<block_type::size;i++)
				{
					first_block->elem[i] = sorted_first_block->elem[i];
				}
				wait_all(reqs,2);
				
				req = first_block->write(first_bid);
				
				for(i=0;i<last_.block_offset();i++)
				{
					last_block->elem[i] = sorted_last_block->elem[i];
				}
				
				req->wait();
				
				
				req = last_block->write(last_bid);
				
				mng->delete_block(out->begin()->bid);
				mng->delete_block((*out)[out->size() - 1].bid);
				
				*first_.bid() = first_bid;
				*last_.bid() = last_bid; 
				
				typename run_type::iterator it = out->begin(); it++;
				typename ExtIterator_::bids_container_iterator cur_bid = first_.bid(); cur_bid ++;
				
				for(;cur_bid != last_.bid(); cur_bid++,it++)
				{
					*cur_bid = (*it).bid;
				}
				
				delete first_block;
				delete sorted_first_block;
				delete sorted_last_block;
				delete [] reqs;
				delete out;
				
				req->wait();
				
				delete last_block;
			}
			else
			{
				// first element resides
				// not in the beginning of the block
				
				typename ExtIterator_::block_type * first_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::bid_type first_bid;
				request_ptr req;
				
				req = first_block->read(*first_.bid());
				mng->new_blocks( FR(), &first_bid,(&first_bid) + 1); // try to overlap
				req->wait();
				
				
				unsigned i=0;
				for(;i<first_.block_offset();i++)
				{
					first_block->elem[i] = keyobj.min_value();
				}
				
				req = first_block->write(first_bid);
				
				n=last_.bid() - first_.bid();
				
				std::swap(first_bid,*first_.bid());
				
				req->wait();
				
				delete first_block;

				run_type * out =
						ksort_blocks<
													typename ExtIterator_::block_type,
													typename ExtIterator_::vector_type::alloc_strategy,
													typename ExtIterator_::bids_container_iterator,
													KeyExtractor_>
														 (first_.bid(),n,M__/block_type::raw_size,keyobj);
					
				
				first_block = new typename ExtIterator_::block_type;
				
				typename ExtIterator_::block_type * sorted_first_block = new typename ExtIterator_::block_type;
	
				request_ptr * reqs = new request_ptr [2];
				
				reqs[0] = first_block->read(first_bid);
				reqs[1] = sorted_first_block->read((*(out->begin())).bid);
				wait_all(reqs,2);
				
				for(i=first_.block_offset();i<block_type::size;i++)
				{
					first_block->elem[i] = sorted_first_block->elem[i];
				}
				
				req = first_block->write(first_bid);
				
				mng->delete_block(out->begin()->bid);
				
				*first_.bid() = first_bid;
				
				typename run_type::iterator it = out->begin(); it++;
				typename ExtIterator_::bids_container_iterator cur_bid = first_.bid(); cur_bid ++;
				
				for(;cur_bid != last_.bid(); cur_bid++,it++)
				{
					*cur_bid = (*it).bid;
				}
				
				*cur_bid = (*it).bid;
				
				delete sorted_first_block;
				delete [] reqs;
				delete out;
				
				req->wait();
				
				delete first_block;
				
			}
			
		}
		else
		{
			if(last_.block_offset()) // last element resides
																// not in the beginning of the block
			{
				typename ExtIterator_::block_type * last_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::bid_type last_bid;
				request_ptr req;
				unsigned i;
				
				req = last_block->read(*last_.bid());
				mng->new_blocks( FR(), &last_bid,(&last_bid) + 1);
				req->wait();
			
				for(unsigned i=last_.block_offset(); i < block_type::size;i++)
				{
					last_block->elem[i] = keyobj.max_value();
				}
				
				req = last_block->write(last_bid);
				
				n=last_.bid() - first_.bid() + 1;
				
				std::swap(last_bid,*last_.bid());
				
				req->wait();
				
				delete last_block;

				run_type * out =
						ksort_blocks<	
													typename ExtIterator_::block_type,
													typename ExtIterator_::vector_type::alloc_strategy,
													typename ExtIterator_::bids_container_iterator,
													KeyExtractor_>
														 (first_.bid(),n,M__/block_type::raw_size,keyobj);
					
				
				last_block = new typename ExtIterator_::block_type;
				typename ExtIterator_::block_type * sorted_last_block = new typename ExtIterator_::block_type;
				request_ptr * reqs = new request_ptr [2];
				
				reqs[0] = last_block->read(last_bid);
				reqs[1] = sorted_last_block->read( ((*out)[out->size() - 1]).bid);
				wait_all(reqs,2);
				
				for(i=0;i<last_.block_offset();i++)
				{
					last_block->elem[i] = sorted_last_block->elem[i];
				}
				
				req = last_block->write(last_bid);
				
				mng->delete_block((*out)[out->size() - 1].bid);
				
				*last_.bid() = last_bid; 
				
				typename run_type::iterator it = out->begin();
				typename ExtIterator_::bids_container_iterator cur_bid = first_.bid();
				
				for(;cur_bid != last_.bid(); cur_bid++,it++)
				{
					*cur_bid = (*it).bid;
				}
				
				delete sorted_last_block;
				delete [] reqs;
				delete out;
				
				req->wait();
				
				delete last_block;
			}
			else
			{
				// first and last element resine in the beginning of blocks 
				n = last_.bid() - first_.bid();
				
				run_type * out =
						ksort_blocks<	
													typename ExtIterator_::block_type,
													typename ExtIterator_::vector_type::alloc_strategy,
													typename ExtIterator_::bids_container_iterator,
													KeyExtractor_>
														 (first_.bid(),n,M__/block_type::raw_size,keyobj);
				
				typename run_type::iterator it = out->begin();
				typename ExtIterator_::bids_container_iterator cur_bid = first_.bid();
				
				for(;cur_bid != last_.bid(); cur_bid++,it++)
				{
					*cur_bid = (*it).bid;
				}
				
			}
		}
		
	}
	
	#ifdef STXXL_CHECK_ORDER_IN_SORTS
	assert(stxxl::is_sorted(first_,last_,key_comparison<value_type,KeyExtractor_>()));
	#endif
};

template<typename record_type>
struct ksort_defaultkey
{
  typedef typename record_type::key_type key_type;
  key_type operator() (const record_type & obj)
  {
    return obj.key();
  }
  record_type max_value()
  {
    return record_type::max_value();
  }
  record_type min_value()
  {
    return record_type::min_value();
  }
};
  

//! \brief External sorting routine for records with keys
//! \param first_ object of model of \c ext_random_access_iterator concept
//! \param last_ object of model of \c ext_random_access_iterator concept
//! \param M__ amount of buffers for internal use
//! \remark Implements external merge sort described in [Dementiev & Sanders'03]
//! \remark Order in the result is non-stable
/*!
  Record's type must:
   - provide \b max_value method that returns an object that is \b greater than all
   other objects of user type ,
   - provide \b min_value method that returns an object that is \b less than all 
   other objects of user type ,
   - \b operator \b < that must define strict weak ordering on record's values
    (<A HREF="http://www.sgi.com/tech/stl/StrictWeakOrdering.html">see what it is</A>).
*/
template <typename ExtIterator_>
void ksort(ExtIterator_ first_, ExtIterator_ last_,unsigned M__)
{
  ksort(first_,last_,
    ksort_defaultkey<typename ExtIterator_::vector_type::value_type>(),M__);
}


//! \}

__STXXL_END_NAMESPACE

#endif
