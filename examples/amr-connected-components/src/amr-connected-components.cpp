// uncomment to switch off sparsification
//#define REEBER_NO_SPARSIFICATION

//#define REEBER_DO_DETAILED_TIMING
#define REEBER_EXTRA_INTEGRAL

#include "reeber-real.h"

// to print nice backtrace on segfault signal
#include <signal.h>
#include <execinfo.h>
#include <cxxabi.h>

#include <AMReX.H>

#include <diy/master.hpp>
#include <diy/io/block.hpp>
#include <diy/io/shared.hpp>
#include <diy/decomposition.hpp>

#include <dlog/stats.h>
#include <dlog/log.h>
#include <opts/opts.h>
#include <error.h>
#include <AMReX_Geometry.H>

#include "../../amr-merge-tree/include/fab-block.h"
#include "fab-cc-block.h"
#include "reader-interfaces.h"

#include <output-persistence.h>

#include "../../amr-merge-tree/include/read-npy.h"
#include "../../amr-merge-tree/include/read-hdf5.h"

#include "amr-plot-reader.h"
#include "amr-connected-components-complex.h"

#include <algorithm>

// block-independent types
using AMRLink = diy::AMRLink;

using BoolVector = diy::RegularDecomposer<diy::DiscreteBounds>::BoolVector;

using Bounds = diy::DiscreteBounds;
using AmrVertexId = r::AmrVertexId;
using AmrEdge = reeber::AmrEdge;

#define DIM 3

using FabBlockR = FabBlock<Real, DIM>;

using Block = FabComponentBlock<Real, DIM>;
using Vertex = Block::Vertex;
using Component = Block::Component;
using MaskedBox = Block::MaskedBox;
using GidVector = Block::GidVector;
using GidSet = Block::GidSet;

using TripletMergeTree = Block::TripletMergeTree;
using Neighbor = TripletMergeTree::Neighbor;

struct IsAmrVertexLocal
{
    bool operator()(const Block& b, const Neighbor& from) const
    {
        return from->vertex.gid == b.gid;
    }
};

template<class Real, class LocalFunctor>
struct ComponentDiagramsFunctor
{

    ComponentDiagramsFunctor(Block* b, const LocalFunctor& lf)
            :
            block_(b),
            negate_(b->get_merge_tree().negate()),
            ignore_zero_persistence_(true),
            test_local(lf)
    { }

    void operator()(Neighbor from, Neighbor through, Neighbor to) const
    {
        if (!test_local(*block_, from))
        {
            return;
        }

        AmrVertexId current_vertex = from->vertex;

        Real birth_time = from->value;
        Real death_time = through->value;

        if (ignore_zero_persistence_ and birth_time == death_time)
        {
            return;
        }

        AmrVertexId root = block_->vertex_to_deepest_[current_vertex];
        block_->local_diagrams_[root].emplace_back(birth_time, death_time);
    }

    Block* block_;
    const bool negate_;
    const bool ignore_zero_persistence_;
    LocalFunctor test_local;
};

using OutputPairsR = OutputPairs<Block, IsAmrVertexLocal>;

inline bool file_exists(const std::string& s)
{
    std::ifstream ifs(s);
    return ifs.good();

}

inline bool ends_with(const std::string& s, const std::string& suffix)
{
    if (suffix.size() > s.size())
    {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

void read_from_file(std::string infn,
        std::vector<std::string> all_var_names, // HDF5 only: all fields that will be read from plotfile
        int n_mt_vars,                          // sum of first n_mt_vars in all_var_names will be stored in fab of FabBlock,
                                                // for each variable listed in all_var_names FabBlock will have an extra GridRef
        diy::mpi::communicator& world,
        diy::Master& master_reader,
        diy::ContiguousAssigner& assigner,
        diy::MemoryBuffer& header,
        diy::DiscreteBounds& domain,
        bool split,
        int nblocks,
        BoolVector wrap)
{

    //std::cout << "Running: " << "read_from_file" << std::endl;

    if (not file_exists(infn))
    {
        throw std::runtime_error("Cannot read file " + infn);
    }

    if (ends_with(infn, ".npy"))
    {
        read_from_npy_file<DIM>(infn, world, nblocks, master_reader, assigner, header, domain, wrap);
    } else if (ends_with(infn, ".h5") || ends_with(infn, ".hdf5"))
    {
        read_from_hdf5_file(infn, all_var_names, n_mt_vars, world, nblocks, master_reader, assigner, header, domain);
    } else
    {
        if (split)
        {
            diy::io::split::read_blocks(infn, world, assigner, master_reader, header, FabBlockR::load);
        } else
        {
            diy::io::read_blocks(infn, world, assigner, master_reader, header, FabBlockR::load);
        }
        diy::load(header, domain);
    }
}

// wang: computes merge tree???
void create_fab_cc_blocks(const diy::mpi::communicator& world, int in_memory, int threads, Real rho, bool absolute,
        bool negate, bool wrap, const diy::FileStorage& storage, diy::Master& master_reader, diy::Master& master,
        const Real cell_volume, const diy::DiscreteBounds& domain)
{
    // copy FabBlocks to FabComponentBlocks
    // in FabTmtConstructor mask will be set and local trees will be computed
    // FabBlock can be safely discarded afterwards
    //std::cout << "Running: " << "create_fab_cc_blocks" << std::endl;

    master_reader.foreach(
            [&master, domain, rho, negate, wrap, absolute, cell_volume](FabBlockR* b, const diy::Master::ProxyWithLink& cp) {
                auto* l = static_cast<AMRLink*>(cp.link());
                AMRLink* new_link = new AMRLink(*l);

                // prepare neighbor box info to save in MaskedBox
                // TODO: refinment vector
                int local_ref = l->refinement()[0];
                int local_lev = l->level();

                master.add(cp.gid(),
                        new Block(b->fab, b->extra_names_, b->extra_fabs_, local_ref, local_lev, domain,
                                l->bounds(),
                                l->core(), cp.gid(),
                                new_link, rho, negate, wrap, absolute, cell_volume),
                        new_link);

            });
}

void write_tree_blocks(const diy::mpi::communicator& world, bool split, const std::string& output_filename,
        diy::Master& master)
{
    
    //std::cout << "Running: " << "write_tree_blocks" << std::endl;
    if (output_filename != "none")
    {
        if (!split)
        {
            diy::io::write_blocks(output_filename, world, master);
        } else
        {
            diy::io::split::write_blocks(output_filename, world, master);
        }
    }
}

int main(int argc, char** argv)
{
    diy::mpi::environment env(argc, argv);
    diy::mpi::communicator world;

    int nblocks = world.size();
    std::string prefix = "./DIY.XXXXXX";
    int in_memory = -1;
    int threads = 1;
    std::string profile_path;
    //std::string log_level = "info";
    std::string log_level = "error";

    dlog::add_stream(std::cerr, dlog::severity(log_level))
            << dlog::stamp() << dlog::aux_reporter(world.rank()) << dlog::color_pre() << dlog::level()
            << dlog::color_post() >> dlog::flush();

    // threshold
    Real rho = 81.66;
    Real absolute_rho;
    int min_cells = 10;

    std::string integral_fields = "";
    std::string function_fields = "";

    int n_runs = 1;

    using namespace opts;

    LOG_SEV_IF(world.rank() == 0, info) << "Started, n_runs = " << n_runs << ", size of Real = " << sizeof(Real);

    opts::Options ops(argc, argv);
    ops
            >> Option('b', "blocks", nblocks, "number of blocks to use")
            >> Option('m', "memory", in_memory, "maximum blocks to store in memory")
            >> Option('j', "jobs", threads, "threads to use during the computation")
            >> Option('s', "storage", prefix, "storage prefix")
            >> Option('i', "rho", rho, "iso threshold")
            >> Option('x', "mincells", min_cells, "minimal number of cells to output halo")
            >> Option('f', "function_fields", function_fields, "fields to add for merge tree, separated with , ")
            >> Option(      "integral_fields", integral_fields, "fields to integrate separated with , ")
            >> Option('r', "runs", n_runs, "number of runs")
            >> Option('p', "profile", profile_path, "path to keep the execution profile")
            >> Option('l', "log", log_level, "log level");

    bool absolute =
            ops >> Present('a', "absolute", "use absolute values for thresholds (instead of multiples of mean)");
    bool read_plotfile = ops >> Present("plotfile", "read AMR plotfiles");
    bool negate = ops >> opts::Present('n', "negate", "sweep superlevel sets");
    // ignored for now, wrap is always assumed
    bool wrap = ops >> opts::Present('w', "wrap", "wrap");
    bool split = ops >> opts::Present("split", "use split IO");

    BoolVector wrap_vec { wrap, wrap, wrap };

    bool print_stats = ops >> opts::Present("stats", "print statistics");
    std::string input_filename, output_filename, output_diagrams_filename, output_integral_filename;


    if (ops >> Present('h', "help", "show help message") or
            not(ops >> PosOption(input_filename)) or
            not(ops >> PosOption(output_filename)))
    {
        if (world.rank() == 0)
        {
            fmt::print("Usage: {} INPUT-PLTFILE OUTPUT.mt [OUT_DIAGRAMS] [OUT_INTEGRAL]\n", argv[0]);
            fmt::print("Compute local-global tree from AMR data\n");
            fmt::print("{}", ops);
        }
        return 1;
    }

    std::vector<std::string> all_var_names;
    std::vector<std::string> integral_var_names;
    int n_mt_vars;
    Real cell_volume = 1.0;

    if (read_plotfile || (ends_with(input_filename, ".h5") || ends_with(input_filename, ".hdf5")))
    {
        std::vector<std::string> function_var_names = split_by_delim(function_fields, ',');  //{"particle_mass_density", "density"};
        n_mt_vars = function_var_names.size();
        if (integral_fields.empty())
        {
            all_var_names = function_var_names;
        } else
        {
            all_var_names = function_var_names;
            integral_var_names = split_by_delim(integral_fields, ',');  //{"particle_mass_density", "density"};
            for(std::string i_name : integral_var_names)
            {
                if (std::find(function_var_names.begin(), function_var_names.end(), i_name) == function_var_names.end())
                {
                    all_var_names.push_back(i_name);
                }
            }
        }
    } else
    {
        // if reading numpy, ignore field names,
        // pretend that numpy contains particle density, and there are no extra fields
        all_var_names.clear();
        all_var_names.push_back("particle_mass_density");
        n_mt_vars = all_var_names.size();
    }
    
    //std::cout << "integral_var_names: " << all_var_names[0] << std::endl;

    LOG_SEV_IF(world.rank() == 0, info) << "Reading fields: " << container_to_string(all_var_names) << ", fields to sum = " << n_mt_vars;
    dlog::flush();


    bool write_diag = (ops >> PosOption(output_diagrams_filename)) and (output_diagrams_filename != "none");
    bool write_integral = (ops >> PosOption(output_integral_filename)) and (output_integral_filename != "none");

    //std::cout << "output_diagrams_filename: " << output_diagrams_filename << std::endl;
    diy::FileStorage storage(prefix);

    diy::Master master_reader(world, 1, in_memory, &FabBlockR::create, &FabBlockR::destroy);
    diy::ContiguousAssigner assigner(world.size(), nblocks);
    diy::MemoryBuffer header;
    diy::DiscreteBounds domain(DIM);

    dlog::Timer timer;
    dlog::Timer timer_all;
    LOG_SEV_IF(world.rank() == 0, info) << "Starting computation, input_filename = " << input_filename << ", nblocks = "
                                        << nblocks
                                        << ", rho = " << rho;
    dlog::flush();


#ifdef REEBER_DO_DETAILED_TIMING
    // detailed timings
    using DurationType = decltype(timer.elapsed());

    dlog::Timer timer_send;
    dlog::Timer timer_receieve;
    dlog::Timer timer_cc_exchange;

    DurationType time_to_construct_blocks;
    DurationType time_to_init_blocks;
    DurationType time_to_get_average;
    DurationType cc_send_time = 0;
    DurationType cc_receive_time = 0;
    DurationType cc_exchange_1_time = 0;
    DurationType cc_exchange_2_time = 0;
    DurationType time_to_delete_low_edges;


    DurationType min_time_to_construct_blocks;
    DurationType min_time_to_init_blocks;
    DurationType min_time_to_get_average;
    DurationType min_cc_send_time;
    DurationType min_cc_receive_time;
    DurationType min_cc_exchange_1_time;
    DurationType min_cc_exchange_2_time;
    DurationType min_time_to_delete_low_edges;
    DurationType min_time_to_compute_components;
    DurationType min_time_to_copy_nodes;

    DurationType max_time_to_construct_blocks;
    DurationType max_time_to_init_blocks;
    DurationType max_time_to_get_average;
    DurationType max_cc_send_time;
    DurationType max_cc_receive_time;
    DurationType max_cc_exchange_1_time;
    DurationType max_cc_exchange_2_time;
    DurationType max_time_to_delete_low_edges;
    DurationType max_time_to_compute_components;
    DurationType max_time_to_copy_nodes;
#endif

    if (read_plotfile)
    {
        LOG_SEV_IF(world.rank() == 0, info) << "Reading plotfile, all_var_names = " << container_to_string(all_var_names) << ", n_mt_vars = " << n_mt_vars;

        read_amr_plotfile(input_filename, all_var_names, n_mt_vars, world, nblocks, master_reader, header, cell_volume, domain);
    } else
    {
        read_from_file(input_filename, all_var_names, n_mt_vars, world, master_reader, assigner, header, domain, split, nblocks, wrap_vec);
    }

    auto time_to_read_data = timer.elapsed();
    dlog::flush();

    //LOG_SEV_IF(world.rank() == 0, info) << "Output_filename: " << output_filename;
    LOG_SEV_IF(world.rank() == 0, info) << "Data read, local size = " << master_reader.size();
    LOG_SEV_IF(world.rank() == 0, info) << "Time to read data:       " << dlog::clock_to_string(timer.elapsed());
    dlog::flush();

    timer.restart();

    for(int n_run = 0; n_run < n_runs; ++n_run)
    {
        world.barrier();
        timer.restart();
        timer_all.restart();

        diy::Master master(world, threads, in_memory, &Block::create, &Block::destroy, &storage, &Block::save,
            &Block::load);

        create_fab_cc_blocks(world, in_memory, threads, rho, absolute, negate, wrap, storage, master_reader, master, cell_volume, domain);

        auto time_for_local_computation = timer.elapsed();


#ifdef REEBER_DO_DETAILED_TIMING
        time_to_construct_blocks = timer.elapsed();
#endif

        Real mean = std::numeric_limits<Real>::min();
        Real total_sum;

        if (absolute)
        {
            absolute_rho = rho;
            LOG_SEV_IF(world.rank() == 0, info) << "Time to compute local trees and components:  "
                                                << dlog::clock_to_string(timer.elapsed());
        } else
        {
            LOG_SEV_IF(world.rank() == 0, info) << "Time to construct FabComponentBlocks: "
                                                << dlog::clock_to_string(timer.elapsed());
            dlog::flush();
            timer.restart();

            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp) {
                cp.collectives()->clear();
                cp.all_reduce(b->sum_ * b->scaling_factor(), std::plus<Real>());
                cp.all_reduce(static_cast<Real>(b->n_unmasked_) * b->scaling_factor(), std::plus<Real>());
            });

            master.exchange();

            const diy::Master::ProxyWithLink& proxy = master.proxy(master.loaded_block());

            total_sum = proxy.get<Real>();
            Real total_unmasked = proxy.get<Real>();

            mean = total_sum / total_unmasked;
	    //std::cout << "total sum = " << total_sum << std::endl;

            absolute_rho = rho * mean;                                            // now rho contains absolute threshold
#ifdef REEBER_DO_DETAILED_TIMING
            time_to_get_average = timer.elapsed();
#endif

            LOG_SEV_IF(world.rank() == 0, info) << "Total sum = " << total_sum << ", total_unmasked = "
                                                << total_unmasked;

            LOG_SEV_IF(world.rank() == 0, info) << "Average = " << mean << ", rho = " << rho
                                                << ", absolute_rho = " << absolute_rho
                                                << ", time to compute average: "
                                                << dlog::clock_to_string(timer.elapsed());

            if (mean < 0 or std::isnan(mean) or std::isinf(mean) or mean > 1e+40)
            {
                LOG_SEV_IF(world.rank() == 0, error) << "Bad average = " << mean << ", do not proceed";
                if (read_plotfile)
                    amrex::Finalize();
                return 1;
            }

            time_for_local_computation += timer.elapsed();
            dlog::flush();
            timer.restart();

            long int local_active = 0;
            master.foreach([absolute_rho, &local_active](Block* b, const diy::Master::ProxyWithLink& cp)
            {
                AMRLink* l = static_cast<AMRLink*>(cp.link());
                b->init(absolute_rho, l, true);
                cp.collectives()->clear();
                local_active += b->n_active_;
            });

            dlog::flush();

#ifdef REEBER_DO_DETAILED_TIMING
            time_to_init_blocks = timer.elapsed();
#endif

            LOG_SEV_IF(world.rank() == 0, info) << "Time to initialize FabComponentBlocks (low vertices, local trees, components, outgoing edges): " << timer.elapsed();
            time_for_local_computation += timer.elapsed();
        }

        dlog::flush();

        // for debug
        {
            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp) {
                cp.collectives()->clear();
                cp.all_reduce(b->sum_low_ * b->scaling_factor(), std::plus<Real>());
                cp.all_reduce(b->sum_active_ * b->scaling_factor(), std::plus<Real>());
            });

            master.exchange();

            const diy::Master::ProxyWithLink& proxy = master.proxy(master.loaded_block());

            Real total_sum_low = proxy.get<Real>();
            Real total_sum_active = proxy.get<Real>();
            LOG_SEV_IF(world.rank() == 0, info) << "Sum of active values  = " << total_sum_active
                                                << ", sum of low = " << total_sum_low
                                                << ", total sum = " << total_sum_active + total_sum_low;

            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp)
            {
                cp.collectives()->clear();
            });

        }

        timer.restart();

        int global_n_undone = 1;

        master.foreach(&send_edges_to_neighbors_cc<Real, DIM>);
        master.exchange();
        master.foreach(&delete_low_edges_cc<Real, DIM>);

#ifdef REEBER_DO_DETAILED_TIMING
        time_to_delete_low_edges = timer.elapsed();
#endif

        LOG_SEV_IF(world.rank() == 0, info) << "edges symmetrized, time elapsed " << timer.elapsed();
        auto time_for_communication = timer.elapsed();
        dlog::flush();
        timer.restart();

#ifdef REEBER_DO_DETAILED_TIMING
        cc_exchange_2_time = 0;
        cc_exchange_1_time = 0;
        cc_receive_time = 0;
        cc_send_time = 0;
#endif

        int rounds = 0;
        while(global_n_undone)
        {
            rounds++;

#ifdef REEBER_DO_DETAILED_TIMING
            timer_send.restart();
#endif

            master.foreach(&amr_cc_send<Real, DIM>);

#ifdef REEBER_DO_DETAILED_TIMING
            cc_send_time += timer_send.elapsed();
            timer_cc_exchange.restart();
#endif

            master.exchange();

#ifdef REEBER_DO_DETAILED_TIMING
            cc_exchange_1_time += timer_cc_exchange.elapsed();
            timer_receieve.restart();
#endif

            master.foreach(&amr_cc_receive<Real, DIM>);

#ifdef REEBER_DO_DETAILED_TIMING
            cc_receive_time += timer_receieve.elapsed();
#endif

            LOG_SEV_IF(world.rank() == 0, info) << "MASTER round " << rounds << ", get OK";
            dlog::flush();

#ifdef REEBER_DO_DETAILED_TIMING
            timer_cc_exchange.restart();
#endif
            master.exchange();
#ifdef REEBER_DO_DETAILED_TIMING
            cc_exchange_2_time += timer_cc_exchange.elapsed();
#endif

            global_n_undone = master.proxy(master.loaded_block()).read<int>();

            //LOG_SEV_IF(world.rank() == 0, info) << "MASTER round " << rounds << ", collectives exchange OK";
            // to compute total number of undone blocks

            LOG_SEV_IF(world.rank() == 0, info) << "MASTER round " << rounds << ", global_n_undone = "
                                                << global_n_undone;

            if (print_stats)
            {
                int local_n_undone = 0;
                master.foreach(
                        [&local_n_undone](Block* b, const diy::Master::ProxyWithLink& cp) {
                            local_n_undone += (b->done_ != 1);
                        });

                LOG_SEV(info) << "STAT MASTER round " << rounds << ", rank = " << world.rank() << ", local_n_undone = "
                              << local_n_undone;
            }
            dlog::flush();
        }

        // here merge tree computation stops
        // sync to measure runtime
        world.barrier();
        auto time_total_computation = timer_all.elapsed();

        //    fmt::print("world.rank = {}, time for exchange = {}\n", world.rank(), dlog::clock_to_string(timer.elapsed()));

        LOG_SEV_IF(world.rank() == 0, info) << "Time for exchange:  " << dlog::clock_to_string(timer.elapsed());
        LOG_SEV_IF(world.rank() == 0, info) << "Total time for computation:  " << time_total_computation;
        time_for_communication += timer.elapsed();
        dlog::flush();
        timer.restart();

        // save the result
        write_tree_blocks(world, split, output_filename, master);

        LOG_SEV_IF(world.rank() == 0, info) << "Time to write tree:  " << dlog::clock_to_string(timer.elapsed());
        auto time_for_output = timer.elapsed();
        dlog::flush();
        timer.restart();

        bool verbose = false;

        if (write_diag)
        {
            bool ignore_zero_persistence = true;
            OutputPairsR::ExtraInfo extra(output_diagrams_filename, verbose, world);
            IsAmrVertexLocal test_local;
            master.foreach(
                    [&extra, &test_local, ignore_zero_persistence, absolute_rho](Block* b,
                            const diy::Master::ProxyWithLink& cp)
                    {
                        output_persistence(b, cp, &extra, test_local, absolute_rho, ignore_zero_persistence);
                        dlog::flush();
                    });
        }

        LOG_SEV_IF(world.rank() == 0, info) << "Time to write diagrams:  " << dlog::clock_to_string(timer.elapsed());
        time_for_output += timer.elapsed();
        dlog::flush();
        timer.restart();

#ifdef REEBER_EXTRA_INTEGRAL
        if (write_integral)
        {
            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp)
            {
                b->compute_final_connected_components();
                b->compute_local_integral();
                b->multiply_integral_by_cell_volume();
            });

            LOG_SEV_IF(world.rank() == 0, info) << "Local integrals computed\n";
            dlog::flush();
            world.barrier();

	    // wang: Output vertices associated with their merge tree and root coordinates
	    std::string dataname=input_filename;
	    std::replace(dataname.begin(), dataname.end(), '/', '_');
	    std::string v2h = all_var_names[0];
	    std::replace(v2h.begin(), v2h.end(), '/', '_');
	    std::string v2h_filename = dataname+"_"+v2h+"_v2h.txt";
	    std::ofstream vertex_output(v2h_filename);
            //master.foreach([&vertex_output](Block* b, const diy::Master::ProxyWithLink& cp) {
	    master.foreach([&vertex_output, absolute_rho, min_cells](Block* b, const diy::Master::ProxyWithLink& cp) {
	        //int tcount = 0;
                for (const auto& vertex_root_pair : b->vertex_to_deepest_) {
	            AmrVertexId vertex = vertex_root_pair.first;  // Vertex ID
                    AmrVertexId root = vertex_root_pair.second;  // Root of the merge tree

                    if (root.gid != b->gid)
                        continue;
	    	    
	    	// Get the cell count associated with this root
                    const auto& values = b->local_integral_.at(root);

                    // Apply filtering based on rho and min_cells
                    Real n_cells = values.at("n_cells");
                    if (n_cells < min_cells) {
                        continue; // Skip if it doesn't meet the criteria
                    }

	    	// Get global position of the root 
                    auto root_position = coarsen_point(b->local_.global_position(root), b->refinement(), 1);
	    	auto vertex_position = coarsen_point(b->local_.global_position(vertex), b->refinement(), 1);
	    	//tcount += 1;
	    	//auto root_position = b->local_.global_position(root);
	    	// Output vertex, root, and root coordinates
                //vertex_output << "Vertex: " << vertex
                //              << " belongs to tree rooted at: " << root
                //              << " (Root Coordinates: " << root_position[0] << ", "
	    	//vertex_output << "Vertex " << vertex_position[0] << ", " << vertex_position[1] << ", " << vertex_position[2] 
                //                  << " at Root " << root_position[0] << ", " << root_position[1] << ", " << root_position[2] << "\n";
		vertex_output << vertex_position[0] << " " << vertex_position[1] << " " << vertex_position[2] << " "
			      << root_position[0] << " " << root_position[1] << " " << root_position[2] << "\n";
                }
	        //std::cout << "tree_count="<< tcount << std::endl;
            });
            vertex_output.close();

	    // wang: this is the file holding the halo data
            diy::io::SharedOutFile ofs(output_integral_filename, world);
	    //std::cout << "output_integral_filename: " << output_integral_filename << std::endl;
            std::cout << "inputfilename: " << input_filename << " " << "field names: " << all_var_names[0] << ", output v2h file: " << v2h_filename << std::endl; 

           //fmt::print(ofs, "{} {} {} {} {} {} {} {} {} {} {}\n",
           //        n_vertices_sf,
           //        n_vertices,
           //        root,
           //        domain_box.index(b->local_.global_position(root)), // TODO: fix for non-flat AMR
           //        b->local_.global_position(root),
           //        vx, vy, vz,
           //        m_gas, m_particles, m_total);

	    // wang: for each thread/process, write own halo data
            master.foreach(
                    [&world, &ofs, domain, min_cells, integral_var_names](Block* b, const diy::Master::ProxyWithLink& cp)
                    {
                        diy::Point<int, 3> domain_shape;
                        for(int i = 0; i < 3; ++i)
                        {
                            domain_shape[i] = domain.max[i] - domain.min[i] + 1;
                        }

			//std::cout << "domain shape: " << domain_shape[0] << " " << domain_shape[1] << " " << domain_shape[2] << std::endl; // 512 512 512

                        diy::GridRef<void*, 3> domain_box(nullptr, domain_shape, /* c_order = */ false);

                        // local integral already stores number of vertices (set in init)
                        // so we add it here to the list of fields just to print it

                        std::vector<std::string> integral_vars = b->extra_names_;

                        integral_vars.insert(integral_vars.begin(), std::string("total_mass"));
                        integral_vars.insert(integral_vars.begin(), std::string("n_vertices"));
                        integral_vars.insert(integral_vars.begin(), std::string("n_cells"));

                        LOG_SEV_IF(world.rank() == 0, debug) << "integral_vars:  " << container_to_string(integral_vars);

                        bool print_header = false;
                        if (print_header)
                        {
                            std::string integral_header = "# id x y z  ";
                            for(auto s : integral_vars)
                            {
                                integral_header += s;
                                integral_header += " ";
                            }
                            integral_header += "\n";

                            fmt::print(ofs, integral_header);
                        }

			//int hcount = 0;
                        for(const auto& root_values_pair : b->local_integral_)
                        {
                            AmrVertexId root = root_values_pair.first;
                            if (root.gid != b->gid)
                                continue;

                            auto& values = root_values_pair.second;

                            Real n_cells = values.at("n_cells");

                            if (n_cells < min_cells)
                                continue;

			    // wang: root_position are the coordinates of the halo center: x y z
			    // wang: domain_box.index produces the index of the halo
			    // wang: b->pretty_integral contains the values of **n_vertices** and **total_mass**
                            auto root_position = coarsen_point(b->local_.global_position(root), b->refinement(), 1);
			    //std::cout << "root_position: " <<  root_position << ", domain_box.index: " << domain_box.index(root_position) << ", b->pretty_integral: " << b->pretty_integral(root, integral_vars) << std::endl;

			    //hcount += 1;
                            fmt::print(ofs, "{} {} {}\n",
                                    domain_box.index(root_position),
                                    root_position,
                                    b->pretty_integral(root, integral_vars));
                        }
			//std::cout << "halo_count=" << hcount << std::endl;
                    });

            LOG_SEV_IF(world.rank() == 0, info) << "Time to compute and write integral:  "
                                                << dlog::clock_to_string(timer.elapsed());
            time_for_output += timer.elapsed();
            dlog::flush();
            timer.restart();
        }

        // for debug
//#ifdef REEBER_COSMOLOGY_INTEGRAL
//        {
//            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp) {
//                cp.collectives()->clear();
//                cp.all_reduce(b->sum_gas_big_halos_, std::plus<Real>());
//                cp.all_reduce(b->sum_particles_big_halos_, std::plus<Real>());
//                cp.all_reduce(b->sum_total_big_halos_, std::plus<Real>());
//
//                cp.all_reduce(b->sum_gas_small_halos_, std::plus<Real>());
//                cp.all_reduce(b->sum_particles_small_halos_, std::plus<Real>());
//                cp.all_reduce(b->sum_total_small_halos_, std::plus<Real>());
//
//            });
//
//            master.exchange();
//
//            const diy::Master::ProxyWithLink& proxy = master.proxy(master.loaded_block());
//
//            Real total_sum_gas_big = proxy.get<Real>();
//            Real total_sum_particles_big = proxy.get<Real>();
//            Real total_sum_mass_big = proxy.get<Real>();
//
//            Real total_sum_gas_small = proxy.get<Real>();
//            Real total_sum_particles_small = proxy.get<Real>();
//            Real total_sum_mass_small = proxy.get<Real>();
//
//            LOG_SEV_IF(world.rank() == 0, info) << "Big gas sum = " << total_sum_gas_big
//                                                << ", big particles sum = " << total_sum_particles_big
//                                                << ", big mass sum = " << total_sum_mass_big;
//
//            LOG_SEV_IF(world.rank() == 0, info) << "Small gas sum = " << total_sum_gas_small
//                                                << ", small particles sum = " << total_sum_particles_small
//                                                << ", small mass sum = " << total_sum_mass_small;
//
//            LOG_SEV_IF(world.rank() == 0, info) << "Big + small gas sum = " << total_sum_gas_small + total_sum_gas_big
//                                                << ", big + small particles sum = "
//                                                << total_sum_particles_small + total_sum_particles_big
//                                                << ", big + small mass sum = "
//                                                << total_sum_mass_small + total_sum_mass_big;
//
//            world.barrier();
//        }
//#endif

#endif
        dlog::flush();

        world.barrier();

        std::string final_timings = fmt::format("run: {} read: {} local: {} exchange: {} output: {} total: {}\n",
                n_run, time_to_read_data, time_for_local_computation, time_for_communication, time_for_output,
                time_total_computation);
        LOG_SEV_IF(world.rank() == 0, info) << final_timings;

        dlog::flush();
#ifdef REEBER_DO_DETAILED_TIMING
        std::string final_detailed_timings = fmt::format(
                "run: {} construct_blocks = {} init_blocks = {} average = {} delete_low_edges = {} cc_send = {} cc_receive = {} cc_exchange_1 = {} cc_exchange_2 = {}\n",
                n_run, time_to_construct_blocks, time_to_init_blocks, time_to_get_average, time_to_delete_low_edges,
                cc_send_time, cc_receive_time, cc_exchange_1_time, cc_exchange_2_time);
        LOG_SEV_IF(world.rank() == 0, info) << final_detailed_timings;
#endif

        if (print_stats)
        {
            std::size_t max_n_gids = 0;
            std::set<int> gids;
            master.foreach([&max_n_gids, &gids](Block* b, const diy::Master::ProxyWithLink& cp) {
                std::set<int> block_gids;
                for(const Component& c : b->components_)
                {
                    gids.insert(c.current_neighbors().begin(), c.current_neighbors().end());
                    block_gids.insert(c.current_neighbors().begin(), c.current_neighbors().end());
                }
                max_n_gids = std::max(max_n_gids, static_cast<decltype(max_n_gids)>(block_gids.size()));
            });

            LOG_SEV_IF(max_n_gids > 0, info) << "STAT max_n_gids[" << world.rank() << "] = " << max_n_gids;
            LOG_SEV_IF(max_n_gids > 0, info) << "STAT total_n_gids[" << world.rank() << "] = " << gids.size();
            dlog::flush();
            world.barrier();

            LOG_SEV_IF(world.rank() == 0, info) << "STAT sizes = np.array(sizes)";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT max_n_gids = np.array(max_n_gids)";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT total_n_gids = np.array(total_n_gids)";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT hist_array = sizes";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT plt.hist(hist_array, bins = 'auto')";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT plt.title('{} cores'.format(n_cores))";
            LOG_SEV_IF(world.rank() == 0, info) << "STAT plt.show()";
        }

        size_t local_n_active = 0;
        size_t local_n_components = 0;
        size_t local_n_blocks = 0;

        if (true)
        {
            master.foreach([](Block* b, const diy::Master::ProxyWithLink& cp) {
                size_t n_low = b->n_low_;
                size_t n_active = b->n_active_;
                size_t n_masked = b->n_masked_;
                cp.collectives()->clear();
                cp.all_reduce(n_low, std::plus<size_t>());
                cp.all_reduce(n_active, std::plus<size_t>());
                cp.all_reduce(n_masked, std::plus<size_t>());
            });

            master.exchange();

            world.barrier();

            const diy::Master::ProxyWithLink& proxy = master.proxy(master.loaded_block());

            size_t total_n_low = proxy.get<size_t>();
            size_t total_n_active = proxy.get<size_t>();
            size_t total_n_masked = proxy.get<size_t>();

            master.foreach([&local_n_active, &local_n_components, &local_n_blocks](Block* b,
                    const diy::Master::ProxyWithLink& cp) {
                local_n_active += b->n_active_;
                local_n_components += b->components_.size();
                local_n_blocks += 1;
            });

            LOG_SEV_IF(world.rank() == 0, info) << "Total_n_low = " << total_n_low << ", total_n_active = "
                                                << total_n_active << ", total_n_masked = "
                                                << total_n_masked;
            dlog::flush();
            timer.restart();
        }

#ifdef REEBER_DO_DETAILED_TIMING
        if (n_run == n_runs - 1 or n_run == 0)
        {

            master.foreach(
                    [time_to_construct_blocks, time_to_init_blocks, time_to_get_average, cc_send_time, cc_exchange_1_time, cc_receive_time, cc_exchange_2_time](
                            Block* b, const diy::Master::ProxyWithLink& cp) {
                        cp.collectives()->clear();

                        cp.all_reduce(time_to_construct_blocks, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(time_to_init_blocks, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(time_to_get_average, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(cc_send_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(cc_exchange_1_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(cc_receive_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(cc_exchange_2_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->compute_components_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->copy_nodes_time, diy::mpi::maximum<DurationType>());

                        cp.all_reduce(time_to_construct_blocks, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(time_to_init_blocks, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(time_to_get_average, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(cc_send_time, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(cc_exchange_1_time, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(cc_receive_time, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(cc_exchange_2_time, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(b->compute_components_time, diy::mpi::minimum<DurationType>());
                        cp.all_reduce(b->copy_nodes_time, diy::mpi::minimum<DurationType>());

                        cp.all_reduce(b->global_receive_time, diy::mpi::maximum<DurationType>());

                        cp.all_reduce(b->original_sparsify_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->set_low_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->original_components_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->local_tree_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->out_edges_time, diy::mpi::maximum<DurationType>());
                        cp.all_reduce(b->integral_init_time, diy::mpi::maximum<DurationType>());

                    });

            master.exchange();

            const diy::Master::ProxyWithLink& proxy = master.proxy(master.loaded_block());

            max_time_to_construct_blocks = proxy.get<DurationType>();
            max_time_to_init_blocks = proxy.get<DurationType>();
            max_time_to_get_average = proxy.get<DurationType>();
            max_cc_send_time = proxy.get<DurationType>();
            max_cc_exchange_1_time = proxy.get<DurationType>();
            max_cc_receive_time = proxy.get<DurationType>();
            max_cc_exchange_2_time = proxy.get<DurationType>();
            max_time_to_compute_components = proxy.get<DurationType>();
            max_time_to_copy_nodes = proxy.get<DurationType>();

            min_time_to_construct_blocks = proxy.get<DurationType>();
            min_time_to_init_blocks = proxy.get<DurationType>();
            min_time_to_get_average = proxy.get<DurationType>();
            min_cc_send_time = proxy.get<DurationType>();
            min_cc_exchange_1_time = proxy.get<DurationType>();
            min_cc_receive_time = proxy.get<DurationType>();
            min_cc_exchange_2_time = proxy.get<DurationType>();
            min_time_to_compute_components = proxy.get<DurationType>();
            min_time_to_copy_nodes = proxy.get<DurationType>();

            DurationType max_block_receive_time = proxy.get<DurationType>();

            DurationType max_original_sparsify_time = proxy.get<DurationType>();
            DurationType max_set_low_time = proxy.get<DurationType>();
            DurationType max_original_components_time = proxy.get<DurationType>();
            DurationType max_local_tree_time = proxy.get<DurationType>();
            DurationType max_out_edges_time = proxy.get<DurationType>();
            DurationType max_integral_init_time = proxy.get<DurationType>();



            LOG_SEV_IF(world.rank() == 0, info) << "max_time_to_construct_blocks = " << max_time_to_construct_blocks;
            LOG_SEV_IF(world.rank() == 0, info) << "min_time_to_construct_blocks = " << min_time_to_construct_blocks;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_time_to_init_blocks = " << max_time_to_init_blocks;
            LOG_SEV_IF(world.rank() == 0, info) << "min_time_to_init_blocks = " << min_time_to_init_blocks;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_time_to_compute_components = " << max_time_to_compute_components;
            LOG_SEV_IF(world.rank() == 0, info) << "min_time_to_compute_components = " << min_time_to_compute_components;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_time_to_copy_nodes = " << max_time_to_copy_nodes;
            LOG_SEV_IF(world.rank() == 0, info) << "min_time_to_copy_nodes = " << min_time_to_copy_nodes;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_time_to_get_average = " << max_time_to_get_average;
            LOG_SEV_IF(world.rank() == 0, info) << "min_time_to_get_average = " << min_time_to_get_average;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";


            LOG_SEV_IF(world.rank() == 0, info) << "max_cc_send_time = " << max_cc_send_time;
            LOG_SEV_IF(world.rank() == 0, info) << "min_cc_send_time = " << min_cc_send_time;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_cc_exchange_1_time = " << max_cc_exchange_1_time;
            LOG_SEV_IF(world.rank() == 0, info) << "min_cc_exchange_1_time = " << min_cc_exchange_1_time;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";


            LOG_SEV_IF(world.rank() == 0, info) << "max_cc_receive_time = " << max_cc_receive_time;
            LOG_SEV_IF(world.rank() == 0, info) << "min_cc_receive_time = " << min_cc_receive_time;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_cc_exchange_2_time = " << max_cc_exchange_2_time;
            LOG_SEV_IF(world.rank() == 0, info) << "min_cc_exchange_2_time = " << min_cc_exchange_2_time;
            LOG_SEV_IF(world.rank() == 0, info) << "---------------------------------------";

            LOG_SEV_IF(world.rank() == 0, info) << "max_block_receive_time = " << max_block_receive_time;

            LOG_SEV_IF(world.rank() == 0, info) << "max_original_sparsify_time = " << max_original_sparsify_time;
            LOG_SEV_IF(world.rank() == 0, info) << "max_set_low_time = " << max_set_low_time;
            LOG_SEV_IF(world.rank() == 0, info) << "max_original_components_time = " << max_original_components_time;
            LOG_SEV_IF(world.rank() == 0, info) << "max_local_tree_time = " << max_local_tree_time;
            LOG_SEV_IF(world.rank() == 0, info) << "max_out_edges_time = " << max_out_edges_time;
            LOG_SEV_IF(world.rank() == 0, info) << "max_integral_init_time = " << max_integral_init_time;


            if (cc_exchange_2_time == max_cc_exchange_2_time or
                cc_exchange_2_time == min_cc_exchange_1_time or
                cc_receive_time == max_cc_receive_time or
                cc_receive_time == min_cc_receive_time)
            {
                LOG_SEV(info) << "Rank = " << world.rank() << ", cc_exchange_2_time = " << cc_exchange_2_time << ", receive_time = "
                              << cc_receive_time << ", local_blocks = " << local_n_blocks
                              << ", local_n_active = " << local_n_active
                              << ", local_n_components = " << local_n_components;
            }

//             if (/*cc_exchange_2_time == min_cc_exchange_1_time or*/
//                    cc_receive_time == max_cc_receive_time
//                    )
            {
                master.foreach([max_block_receive_time](Block* b, const diy::Master::ProxyWithLink& cp) {

                    if (b->global_receive_time != max_block_receive_time)
                        return;

                    if (b->global_receive_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid
                                                                            << ", Total time in receive = "
                                                                            << b->global_receive_time;
                     if (b->process_senders_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid
                                                                            << ", time_to_receive_trees_and_gids = "
                                                                            << b->process_senders_time;
                    if (b->repair_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", repair_time = "
                                                                            << b->repair_time;

                    if (b->merge_call_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", merge_call_time = "
                                                                            << b->merge_call_time;
                    LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", merge_calls = "
                                                                        << b->merge_calls << ", edges in merge " << b->edges_in_merge;
                    if (b->uc_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", uc_time = "
                                                                            << b->uc_time;
                    if (b->comps_loop_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", comps_loop = "
                                      << b->comps_loop_time;

                    if (b->rrtc_loop_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", rrtc_time = "
                                                                            << b->rrtc_loop_time;
                    if (b->ucn_loop_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", ucn_time = "
                                                                            << b->ucn_loop_time;

                    if (b->expand_link_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", expand_link_time = "
                                                                            << b->expand_link_time;
                    if (b->is_done_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", is_done_time = "
                                                                            << b->is_done_time;
                    if (b->collectives_time > 0)
                        LOG_SEV(info) << "MAX RECEIVE TIME details, gid = " << b->gid << ", collectives_time = "
                                                                            << b->collectives_time;
                 });
            }
        } // if for 1st and last runs
#endif
    } // loop over runs

    if (read_plotfile)
    {
        amrex::Finalize();
    }

    return 0;
}
