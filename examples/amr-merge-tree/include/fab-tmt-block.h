#pragma once

#include <utility>
#include <numeric>
#include <boost/functional/hash.hpp>

#include "diy/serialization.hpp"
#include "diy/grid.hpp"
#include "diy/link.hpp"
#include "diy/fmt/format.h"
#include "diy/fmt/ostream.h"
#include "diy/point.hpp"
#include <diy/master.hpp>

#include "reeber/amr-vertex.h"
#include "reeber/triplet-merge-tree.h"
#include "reeber/triplet-merge-tree-serialization.h"
#include "reeber/grid.h"
#include "reeber/grid-serialization.h"
#include "reeber/masked-box.h"
#include "reeber/edges.h"

#include "fab-block.h"
#include "reeber/amr_helper.h"

namespace r = reeber;

template<class Real, unsigned D>
struct FabTmtBlock
{
    using Shape = diy::Point<int, D>;

    using Grid = r::Grid<Real, D>;
    using GridRef = r::GridRef<Real, D>;
    // index of point: first = index inside box, second = index of a box
    using AmrVertexId = r::AmrVertexId;
    using Value = typename Grid::Value;
    using MaskedBox = r::MaskedBox<D>;
    using Vertex = typename MaskedBox::Position;
    using TripletMergeTree = r::TripletMergeTree<r::AmrVertexId, Value>;
    using Size = std::vector<Real>;
    using AmrVertexContainer = std::vector<AmrVertexId>;

    using Neighbor = typename TripletMergeTree::Neighbor;
    using Node = typename TripletMergeTree::Node;
    using VertexNeighborMap =typename TripletMergeTree::VertexNeighborMap;

    using AmrEdge = r::AmrEdge;
    using AmrEdgeContainer = r::AmrEdgeContainer;
    using AmrEdgeSet = std::set<AmrEdge>;
    using VertexEdgesMap = std::map<AmrVertexId, AmrEdgeContainer>;
    using VertexVertexMap = std::map<AmrVertexId, AmrVertexId>;
    using VertexSizeMap = std::map<AmrVertexId, int>;

    using GidContainer = std::set<int>;
    using GidVector = std::vector<int>;

    using RealType = Real;

    using LocalIntegral = std::map<AmrVertexId, Real>;
    using DiagramPoint = std::pair<Real, Real>;
    using Diagram = std::vector<DiagramPoint>;

    template<class Vertex_, class Node>
    struct TmtConnectedComponent
    {
        // types
        using Vertex = Vertex_;
        using Neighbor = Node *;

        // fields
        AmrVertexId root_;
        GidContainer current_neighbors_;
        GidContainer processed_neighbors_;
#ifdef AMR_MT_SEND_COMPONENTS
        AmrEdgeContainer outgoing_edges_;
        TripletMergeTree merge_tree_;
#endif

        // methods

        TmtConnectedComponent()
        {
        }

        template<class EC>
        TmtConnectedComponent(const AmrVertexId& root, const EC& _edges) :
                root_(root)
#ifdef AMR_MT_SEND_COMPONENTS
                , outgoing_edges_(_edges.cbegin(), _edges.cend())
#endif
        {
#ifdef AMR_MT_SEND_COMPONENTS
            bool debug = false;

            fill_current_neighbors();
            if (debug)
                fmt::print("entered TmtConnectedComponentConstructor, root = {}, #edges = {}\n", root,
                           outgoing_edges_.size());
#endif
        }

#ifdef AMR_MT_SEND_COMPONENTS

        template<class EC>
        void add_edges(const EC& more_edges)
        {
            outgoing_edges_.insert(outgoing_edges_.end(), more_edges.cbegin(), more_edges.cend());

            std::transform(more_edges.cbegin(), more_edges.cend(),
                           std::inserter(current_neighbors_, current_neighbors_.begin()),
                           [this](const AmrEdge& e) {
                               assert(std::get<0>(e).gid == this->root_.gid);
                               return std::get<1>(e).gid;
                           });
        }

        void fill_current_neighbors(bool debug = false)
        {
            current_neighbors_.clear();

            std::transform(outgoing_edges_.begin(), outgoing_edges_.end(),
                           std::inserter(current_neighbors_, current_neighbors_.begin()),
                           [this](const AmrEdge& e) {
                               assert(std::get<0>(e).gid == this->root_.gid);
                               assert(std::get<1>(e).gid != this->root_.gid);
                               return std::get<1>(e).gid;
                           });
            if (debug) fmt::print("In fill_current_neighbors for component = {}, current_neighbors_.size = {}\n", root_, current_neighbors_.size());
        }

        template<class EC>
        void adjust_edges(const EC& initial_edges)
        {
            bool debug = false;
            if (outgoing_edges_.empty())
                return;
            std::set<AmrEdge> old_outgoing_edges(outgoing_edges_.begin(), outgoing_edges_.end());
            std::vector<AmrEdge> new_outgoing_edges;
            std::set_intersection(old_outgoing_edges.begin(), old_outgoing_edges.end(), initial_edges.begin(), initial_edges.end(), std::back_inserter(new_outgoing_edges));
            if (debug) fmt::print("In delete_low_edges for component = {}, old outgoing_edges_.size = {}, new = {}\n", root_, outgoing_edges_.size(), new_outgoing_edges.size());
            if (outgoing_edges_.size() > new_outgoing_edges.size())
            {
                outgoing_edges_ = new_outgoing_edges;
                fill_current_neighbors(debug);
            }
        }

        int is_not_done() const
        {
            if (!std::all_of(processed_neighbors_.begin(), processed_neighbors_.end(), [this](int i) { return this->current_neighbors_.count(i) == 1; }))
            {
                fmt::print("Error, gid = {}, processed_neighbours = {}, current_neighbours = {}\n", root_.gid,
                           container_to_string(processed_neighbors_), container_to_string(current_neighbors_));
                throw std::runtime_error("BUG");
            }
            assert(std::all_of(processed_neighbors_.begin(), processed_neighbors_.end(), [this](int i) { return this->current_neighbors_.count(i) == 1; }));
            return current_neighbors_.size() > processed_neighbors_.size();
        }

        bool must_send_to_gid(int gid) const
        {
            return current_neighbors_.count(gid) == 1 and processed_neighbors_.count(gid) == 0;
        }

//        void mark_neighbor_processed(int gid)
//        {
//            assert(current_neighbors_.count(gid));
//            processed_neighbors_.insert(gid);
//        }

#endif


    };

    using Component = TmtConnectedComponent<reeber::AmrVertexId, Node>;


    // data

    int gid;
    MaskedBox local_;
    TripletMergeTree current_merge_tree_;
    TripletMergeTree original_tree_;

    // if relative threshold is given, we cannot determine
    // LOW values in constructor. Instead, we mark all unmasked vertices
    // ACTIVE and save the average of their values in local_sum_ and the number in local_n_unmasked_
    // Pointer to grid data is saved in fab_ and after all blocks exchange their local averages
    // we resume initialization
    Real sum_ { 0 };
    size_t n_unmasked_ { 0 };
    GridRef fab_;

    // this vector is not serialized, because we send trees component-wise
    std::vector<Component> components_;

    diy::DiscreteBounds domain_;

    int done_ { 0 };

    //    // will be changed in each communication round
    //    // only for baseiline algorithm
    //    AmrEdgeSet outgoing_edges_;

    // is pre-computed once. does not change
    // only for baseiline algorithm
    AmrEdgeContainer initial_edges_;

    std::map<int, AmrEdgeContainer> gid_to_outgoing_edges_;

    std::set<int> new_receivers_;
    std::set<int> processed_receivers_;

    GidVector original_link_gids_;

    bool negate_;

    // to store information about local connected component in a serializable way
    VertexVertexMap original_vertex_to_deepest_;
    VertexVertexMap current_vertex_to_deepest_;
    VertexVertexMap final_vertex_to_deepest_;

    std::set<AmrVertexId> original_deepest_;
    std::set<AmrVertexId> current_deepest_;

    // tracking how connected components merge - disjoint sets data structure
    VertexVertexMap components_disjoint_set_parent_;
    VertexSizeMap components_disjoint_set_size_;

    int round_ { 0 };

    // for persistent integral
    LocalIntegral local_integral_;

    // for diagrams of connected components
    std::map<AmrVertexId, Diagram> local_diagrams_;
    // methods

    // simple getters/setters
    const diy::DiscreteBounds& domain() const
    { return domain_; }

    int refinement() const
    { return local_.refinement(); }

    int level() const
    { return local_.level(); }

    const GidVector& get_original_link_gids() const
    { return original_link_gids_; }

    FabTmtBlock(diy::GridRef<Real, D>& fab_grid,
                int _ref,
                int _level,
                const diy::DiscreteBounds& _domain,
                const diy::DiscreteBounds& bounds,
                const diy::DiscreteBounds& core,
                int _gid,
                diy::AMRLink *amr_link,
                Real rho,                                           // threshold for LOW value
                bool _negate,
                bool is_absolute_threshold) :
            gid(_gid),
            local_(project_point<D>(core.min), project_point<D>(core.max), project_point<D>(bounds.min),
                   project_point<D>(bounds.max), _ref, _level, gid, fab_grid.c_order()),
            current_merge_tree_(_negate),
            original_tree_(_negate),
            fab_(fab_grid.data(), fab_grid.shape(), fab_grid.c_order()),
            domain_(_domain),
            processed_receivers_({ gid }),
            negate_(_negate)
    {
        bool debug = false;

        std::string debug_prefix = "FabTmtBlock ctor, gid = " + std::to_string(gid);

        if (debug) fmt::print("{} setting mask\n", debug_prefix);

        diy::for_each(local_.mask_shape(), [this, amr_link, rho, is_absolute_threshold](const Vertex& v) {
            this->set_mask(v, amr_link, rho, is_absolute_threshold);
        });

        //        if (debug) fmt::print("gid = {}, checking mask\n", gid);
        int max_gid = 0;
        for (int i = 0; i < amr_link->size(); ++i)
        {
            max_gid = std::max(max_gid, amr_link->target(i).gid);
        }

        //local_.check_mask_validity(max_gid);

        if (is_absolute_threshold)
        {
            init(rho, amr_link);
        }
    }

    FabTmtBlock() :
            fab_(nullptr, diy::Point<int, D>::zero())
    {}

    void init(Real absolute_rho, diy::AMRLink *amr_link)
    {
        bool debug = false;
        std::string debug_prefix = "In FabTmtBlock::init, gid = " + std::to_string(gid);

        diy::for_each(local_.mask_shape(), [this, absolute_rho](const Vertex& v) {
            this->set_low(v, absolute_rho);
        });

        reeber::compute_merge_tree2(current_merge_tree_, local_, fab_);
        current_merge_tree_.make_deep_copy(original_tree_);

        VertexEdgesMap vertex_to_outgoing_edges;
        compute_outgoing_edges(amr_link, vertex_to_outgoing_edges);

        sparsify_prune_original_tree();

        compute_original_connected_components(vertex_to_outgoing_edges);

        // TODO: delete this? we are going to overwrite this in adjust_outgoing_edges anyway
        for (int i = 0; i < amr_link->size(); ++i)
        {
            if (amr_link->target(i).gid != gid)
            {
                new_receivers_.insert(amr_link->target(i).gid);
                original_link_gids_.push_back(amr_link->target(i).gid);
            }
        }

        if (debug)
            fmt::print("{}, constructed, refinement = {}, level = {}, local = {}, domain.max = {}, #components = {}\n",
                       debug_prefix, refinement(), level(), local_, domain().max, components_.size());
        if (debug)
            fmt::print("{},  constructed, tree.size = {}, new_receivers.size = {}\n",
                       debug_prefix, current_merge_tree_.size(), new_receivers_.size());

        assert(current_merge_tree_.size() >= original_tree_.size());
    }

    void sparsify_prune_original_tree()
    {
        std::unordered_set<AmrVertexId> special;
        for (const AmrEdge& out_edge : get_all_outgoing_edges())
        {
            special.insert(std::get<0>(out_edge));
        }
        r::remove_degree_two(original_tree_, [&special](AmrVertexId u) { return special.find(u) != special.end(); });
        r::sparsify(original_tree_, [&special](AmrVertexId u) { return special.find(u) != special.end(); });
    }

    void sparsify_local_tree()
    {
//        std::unordered_set<AmrVertexId> special;
//        for (const AmrEdge& out_edge : get_all_outgoing_edges())
//        {
//            special.insert(std::get<0>(out_edge));
//        }
////        r::remove_degree_two(original_tree_, [&special](AmrVertexId u) { return special.find(u) != special.end(); });
//        r::sparsify(original_tree_, [&special](AmrVertexId u) { return special.find(u) != special.end(); });
    }


    // compare w.r.t negate_ flag
    bool precedes(Real a, Real b) const;

    bool succeeds(Real a, Real b) const;

    bool precedes_eq(Real a, Real b) const;

    bool succeeds_eq(Real a, Real b) const;

    void set_low(const diy::Point<int, D>& v_bounds,
                 const Real& absolute_rho);


    void set_mask(const diy::Point<int, D>& v_bounds,
                  diy::AMRLink *l,
                  const Real& rho,
                  bool is_absolute_threshold);

    const TripletMergeTree& get_merge_tree() const
    { return current_merge_tree_; }

    // return true, if both edge vertices are in the current neighbourhood
    // no checking of mask is performed, if a vertex is LOW, function will return true.
    // Such an edge must be silently ignored in the merge procedure.
    bool edge_exists(const AmrEdge& e) const;

    // return true, if one of the edge's vertices is inside current neighbourhood
    // and the other is outside
    bool edge_goes_out(const AmrEdge& e) const;

    bool original_deepest_computed(Neighbor n) const { return original_deepest_computed(n->vertex); }

    bool original_deepest_computed(const AmrVertexId& v) const { return original_vertex_to_deepest_.find(v) != original_vertex_to_deepest_.cend(); }

    bool final_deepest_computed(Neighbor n) const { return final_deepest_computed(n->vertex); }

    bool final_deepest_computed(const AmrVertexId& v) const { return final_vertex_to_deepest_.find(v) != final_vertex_to_deepest_.cend(); }

    AmrVertexId original_deepest(Neighbor n) const { return original_deepest(n->vertex); }

    AmrVertexId original_deepest(const AmrVertexId& v) const;

    AmrVertexId final_deepest(Neighbor n) const { return final_deepest(n->vertex); }

    AmrVertexId final_deepest(const AmrVertexId& v) const;


    void set_original_deepest(const AmrVertexId& v, const AmrVertexId& deepest)
    { original_vertex_to_deepest_[v] = deepest; }

    void create_component(const AmrVertexId& deepest_vertex, const AmrEdgeContainer& edges);

    void compute_outgoing_edges(diy::AMRLink *l, VertexEdgesMap& vertex_to_outgoing_edges);

    void compute_original_connected_components(const VertexEdgesMap& vertex_to_outgoing_edges);

    void compute_final_connected_components();

    void delete_low_edges(int sender_gid, AmrEdgeContainer& edges_from_sender);

    void adjust_outgoing_edges();

//    void adjust_original_gids(int sender_gid, FabTmtBlock::GidVector& edges_from_sender);

    // disjoint-sets related methods
    bool are_components_connected(const AmrVertexId& deepest_a, const AmrVertexId& deepest_b);

    bool is_component_connected_to_any_internal(const AmrVertexId& deepest);

    void connect_components(const AmrVertexId& deepest_a, const AmrVertexId& deepest_b);

    void add_component_to_disjoint_sets(const AmrVertexId& deepest_vertex);

#ifdef AMR_MT_SEND_COMPONENTS
    Component& find_component(const AmrVertexId& deepest_vertex);
    void add_received_original_vertices(const VertexVertexMap& received_vertex_to_deepest);
    int are_all_components_done() const;
    std::vector<AmrVertexId> get_current_deepest_vertices() const;
    int n_undone_components() const;
#endif

    int is_done_simple(const std::vector<FabTmtBlock::AmrVertexId>& vertices_to_check);

    void compute_local_integral(Real rho_min, Real rho_max);

    Real scaling_factor() const;

    std::vector<AmrVertexId> get_original_deepest_vertices() const;

    const AmrEdgeContainer& get_all_outgoing_edges()
    { return initial_edges_; }

    // v must be the deepest vertex in a local connected component
    // cannot be const - path compression!
    AmrVertexId find_component_in_disjoint_sets(AmrVertexId v);

//    bool gid_must_be_in_link(int gid) const;

    std::pair<Real, size_t> get_local_stats() const;


    static void *create()
    {
        return new FabTmtBlock;
    }

    static void destroy(void *b)
    {
        delete static_cast<FabTmtBlock *>(b);
    }

    static void save(const void *b, diy::BinaryBuffer& bb);

    static void load(void *b, diy::BinaryBuffer& bb);
};


#include "fab-tmt-block.hpp"
