/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_stack.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "NOD_derived_node_tree.hh"

#include "COM_scheduler.hh"
#include "COM_utilities.hh"

namespace blender::realtime_compositor {

using namespace nodes::derived_node_tree_types;

/* Compute the output node whose result should be computed. The output node is the node marked as
 * NODE_DO_OUTPUT. If multiple types of output nodes are marked, then the preference will be
 * CMP_NODE_COMPOSITE > CMP_NODE_VIEWER > CMP_NODE_SPLITVIEWER. If no output node exists, a null
 * node will be returned. */
static DNode compute_output_node(DerivedNodeTree &tree)
{
  const NodeTreeRef &root_tree = tree.root_context().tree();

  for (const NodeRef *node : root_tree.nodes_by_type("CompositorNodeComposite")) {
    if (node->bnode()->flag & NODE_DO_OUTPUT) {
      return DNode(&tree.root_context(), node);
    }
  }

  for (const NodeRef *node : root_tree.nodes_by_type("CompositorNodeViewer")) {
    if (node->bnode()->flag & NODE_DO_OUTPUT) {
      return DNode(&tree.root_context(), node);
    }
  }

  for (const NodeRef *node : root_tree.nodes_by_type("CompositorNodeSplitViewer")) {
    if (node->bnode()->flag & NODE_DO_OUTPUT) {
      return DNode(&tree.root_context(), node);
    }
  }

  /* No output node found, return a null node. */
  return DNode();
}

/* A type representing a mapping that associates each node with a heuristic estimation of the
 * number of intermediate buffers needed to compute it and all of its dependencies. See the
 * compute_number_of_needed_buffers function for more information. */
using NeededBuffers = Map<DNode, int>;

/* Compute a heuristic estimation of the number of intermediate buffers needed to compute each node
 * and all of its dependencies for all nodes that the given node depends on. The output is a map
 * that maps each node with the number of intermediate buffers needed to compute it and all of its
 * dependencies.
 *
 * Consider a node that takes n number of buffers as an input from a number of node dependencies,
 * which we shall call the input nodes. The node also computes and outputs m number of buffers.
 * In order for the node to compute its output, a number of intermediate buffers will be needed.
 * Since the node takes n buffers and outputs m buffers, then the number of buffers directly
 * needed by the node is (n + m). But each of the input buffers are computed by a node that, in
 * turn, needs a number of buffers to compute its output. So the total number of buffers needed
 * to compute the output of the node is max(n + m, d) where d is the number of buffers needed by
 * the input node that needs the largest number of buffers. We only consider the input node that
 * needs the largest number of buffers, because those buffers can be reused by any input node
 * that needs a lesser number of buffers.
 *
 * Shader nodes, however, are a special case because links between two shader nodes inside the same
 * shader operation don't pass a buffer, but a single value in the compiled shader. So for shader
 * nodes, only inputs and outputs linked to nodes that are not shader nodes should be considered.
 * Note that this might not actually be true, because the compiler may decide to split a shader
 * operation into multiples ones that will pass buffers, but this is not something that can be
 * known at scheduling-time. See the discussion in COM_compile_state.hh, COM_evaluator.hh, and
 * COM_shader_operation.hh for more information. In the node tree shown below, node 4 will have
 * exactly the same number of needed buffers by node 3, because its inputs and outputs are all
 * internally linked in the shader operation.
 *
 *                                      Shader Operation
 *                   +------------------------------------------------------+
 * .------------.    |  .------------.  .------------.      .------------.  |  .------------.
 * |   Node 1   |    |  |   Node 3   |  |   Node 4   |      |   Node 5   |  |  |   Node 6   |
 * |            |----|--|            |--|            |------|            |--|--|            |
 * |            |  .-|--|            |  |            |  .---|            |  |  |            |
 * '------------'  | |  '------------'  '------------'  |   '------------'  |  '------------'
 *                 | +----------------------------------|-------------------+
 * .------------.  |                                    |
 * |   Node 2   |  |                                    |
 * |            |--'------------------------------------'
 * |            |
 * '------------'
 *
 * Note that the computed output is not guaranteed to be accurate, and will not be in most cases.
 * The computation is merely a heuristic estimation that works well in most cases. This is due to a
 * number of reasons:
 * - The node tree is actually a graph that allows output sharing, which is not something that was
 *   taken into consideration in this implementation because it is difficult to correctly consider.
 * - Each node may allocate any number of internal buffers, which is not taken into account in this
 *   implementation because it rarely affects the output and is done by very few nodes.
 * - The compiler may decide to compiler the schedule differently depending on runtime information
 *   which we can merely speculate at scheduling-time as described above. */
static NeededBuffers compute_number_of_needed_buffers(DNode output_node)
{
  NeededBuffers needed_buffers;

  /* A stack of nodes used to traverse the node tree starting from the output node. */
  Stack<DNode> node_stack = {output_node};

  /* Traverse the node tree in a post order depth first manner and compute the number of needed
   * buffers for each node. Post order traversal guarantee that all the node dependencies of each
   * node are computed before it. This is done by pushing all the uncomputed node dependencies to
   * the node stack first and only popping and computing the node when all its node dependencies
   * were computed. */
  while (!node_stack.is_empty()) {
    /* Do not pop the node immediately, as it may turn out that we can't compute its number of
     * needed buffers just yet because its dependencies weren't computed, it will be popped later
     * when needed. */
    DNode &node = node_stack.peek();

    /* Go over the node dependencies connected to the inputs of the node and push them to the node
     * stack if they were not computed already. */
    Set<DNode> pushed_nodes;
    for (const InputSocketRef *input_ref : node->inputs()) {
      const DInputSocket input{node.context(), input_ref};

      /* Get the output linked to the input. If it is null, that means the input is unlinked and
       * has no dependency node. */
      const DOutputSocket output = get_output_linked_to_input(input);
      if (!output) {
        continue;
      }

      /* The node dependency was already computed or pushed before, so skip it. */
      if (needed_buffers.contains(output.node()) || pushed_nodes.contains(output.node())) {
        continue;
      }

      /* The output node needs to be computed, push the node dependency to the node stack and
       * indicate that it was pushed. */
      node_stack.push(output.node());
      pushed_nodes.add_new(output.node());
    }

    /* If any of the node dependencies were pushed, that means that not all of them were computed
     * and consequently we can't compute the number of needed buffers for this node just yet. */
    if (!pushed_nodes.is_empty()) {
      continue;
    }

    /* We don't need to store the result of the pop because we already peeked at it before. */
    node_stack.pop();

    /* Compute the number of buffers that the node takes as an input as well as the number of
     * buffers needed to compute the most demanding of the node dependencies. */
    int number_of_input_buffers = 0;
    int buffers_needed_by_dependencies = 0;
    for (const InputSocketRef *input_ref : node->inputs()) {
      const DInputSocket input{node.context(), input_ref};

      /* Get the output linked to the input. If it is null, that means the input is unlinked.
       * Unlinked inputs do not take a buffer, so skip those inputs. */
      const DOutputSocket output = get_output_linked_to_input(input);
      if (!output) {
        continue;
      }

      /* Since this input is linked, if the link is not between two shader nodes, it means that the
       * node takes a buffer through this input and so we increment the number of input buffers. */
      if (!is_shader_node(node) || !is_shader_node(output.node())) {
        number_of_input_buffers++;
      }

      /* If the number of buffers needed by the node dependency is more than the total number of
       * buffers needed by the dependencies, then update the latter to be the former. This is
       * computing the "d" in the aforementioned equation "max(n + m, d)". */
      const int buffers_needed_by_dependency = needed_buffers.lookup(output.node());
      if (buffers_needed_by_dependency > buffers_needed_by_dependencies) {
        buffers_needed_by_dependencies = buffers_needed_by_dependency;
      }
    }

    /* Compute the number of buffers that will be computed/output by this node. */
    int number_of_output_buffers = 0;
    for (const OutputSocketRef *output_ref : node->outputs()) {
      const DOutputSocket output{node.context(), output_ref};

      /* The output is not linked, it outputs no buffer. */
      if (output->logically_linked_sockets().is_empty()) {
        continue;
      }

      /* If any of the links is not between two shader nodes, it means that the node outputs
       * a buffer through this output and so we increment the number of output buffers. */
      if (!is_output_linked_to_node_conditioned(output, is_shader_node) || !is_shader_node(node)) {
        number_of_output_buffers++;
      }
    }

    /* Compute the heuristic estimation of the number of needed intermediate buffers to compute
     * this node and all of its dependencies. This is computing the aforementioned equation
     * "max(n + m, d)". */
    const int total_buffers = MAX2(number_of_input_buffers + number_of_output_buffers,
                                   buffers_needed_by_dependencies);
    needed_buffers.add(node, total_buffers);
  }

  return needed_buffers;
}

/* There are multiple different possible orders of evaluating a node graph, each of which needs
 * to allocate a number of intermediate buffers to store its intermediate results. It follows
 * that we need to find the evaluation order which uses the least amount of intermediate buffers.
 * For instance, consider a node that takes two input buffers A and B. Each of those buffers is
 * computed through a number of nodes constituting a sub-graph whose root is the node that
 * outputs that buffer. Suppose the number of intermediate buffers needed to compute A and B are
 * N(A) and N(B) respectively and N(A) > N(B). Then evaluating the sub-graph computing A would be
 * a better option than that of B, because had B was computed first, its outputs will need to be
 * stored in extra buffers in addition to the buffers needed by A. The number of buffers needed by
 * each node is estimated as described in the compute_number_of_needed_buffers function.
 *
 * This is a heuristic generalization of the Sethi–Ullman algorithm, a generalization that
 * doesn't always guarantee an optimal evaluation order, as the optimal evaluation order is very
 * difficult to compute, however, this method works well in most cases. Moreover it assumes that
 * all buffers will have roughly the same size, which may not always be the case. */
Schedule compute_schedule(DerivedNodeTree &tree)
{
  Schedule schedule;

  /* Compute the output node whose result should be computed. */
  const DNode output_node = compute_output_node(tree);

  /* No output node, the node tree has no effect, return an empty schedule. */
  if (!output_node) {
    return schedule;
  }

  /* Compute the number of buffers needed by each node connected to the output. */
  const NeededBuffers needed_buffers = compute_number_of_needed_buffers(output_node);

  /* A stack of nodes used to traverse the node tree starting from the output node. */
  Stack<DNode> node_stack = {output_node};

  /* Traverse the node tree in a post order depth first manner, scheduling the nodes in an order
   * informed by the number of buffers needed by each node. Post order traversal guarantee that all
   * the node dependencies of each node are scheduled before it. This is done by pushing all the
   * unscheduled node dependencies to the node stack first and only popping and scheduling the node
   * when all its node dependencies were scheduled. */
  while (!node_stack.is_empty()) {
    /* Do not pop the node immediately, as it may turn out that we can't schedule it just yet
     * because its dependencies weren't scheduled, it will be popped later when needed. */
    DNode &node = node_stack.peek();

    /* Compute the nodes directly connected to the node inputs sorted by their needed buffers such
     * that the node with the lowest number of needed buffers comes first. Note that we actually
     * want the node with the highest number of needed buffers to be schedule first, but since
     * those are pushed to the traversal stack, we need to push them in reverse order. */
    Vector<DNode> sorted_dependency_nodes;
    for (const InputSocketRef *input_ref : node->inputs()) {
      const DInputSocket input{node.context(), input_ref};

      /* Get the output linked to the input. If it is null, that means the input is unlinked and
       * has no dependency node, so skip it. */
      const DOutputSocket output = get_output_linked_to_input(input);
      if (!output) {
        continue;
      }

      /* The dependency node was added before, so skip it. The number of dependency nodes is very
       * small, typically less than 3, so a linear search is okay. */
      if (sorted_dependency_nodes.contains(output.node())) {
        continue;
      }

      /* The dependency node was already schedule, so skip it. */
      if (schedule.contains(output.node())) {
        continue;
      }

      /* Sort in ascending order on insertion, the number of dependency nodes is very small,
       * typically less than 3, so insertion sort is okay. */
      int insertion_position = 0;
      for (int i = 0; i < sorted_dependency_nodes.size(); i++) {
        if (needed_buffers.lookup(output.node()) >
            needed_buffers.lookup(sorted_dependency_nodes[i])) {
          insertion_position++;
        }
        else {
          break;
        }
      }
      sorted_dependency_nodes.insert(insertion_position, output.node());
    }

    /* Push the sorted dependency nodes to the node stack in order. */
    for (const DNode &dependency_node : sorted_dependency_nodes) {
      node_stack.push(dependency_node);
    }

    /* If there are no sorted dependency nodes, that means they were all already scheduled or that
     * none exists in the first place, so we can pop and schedule the node now. */
    if (sorted_dependency_nodes.is_empty()) {
      /* The node might have already been scheduled, so we don't use add_new here and simply don't
       * add it if it was already scheduled. */
      schedule.add(node_stack.pop());
    }
  }

  return schedule;
}

}  // namespace blender::realtime_compositor
