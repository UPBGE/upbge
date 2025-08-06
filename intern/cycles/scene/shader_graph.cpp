/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "scene/shader_graph.h"
#include "scene/attribute.h"
#include "scene/constant_fold.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_nodes.h"

#include "util/algorithm.h"

#include "util/log.h"
#include "util/md5.h"
#include "util/queue.h"

CCL_NAMESPACE_BEGIN

namespace {

bool check_node_inputs_has_links(const ShaderNode *node)
{
  for (const ShaderInput *in : node->inputs) {
    if (in->link) {
      return true;
    }
  }
  return false;
}

bool check_node_inputs_traversed(const ShaderNode *node, const ShaderNodeSet &done)
{
  for (const ShaderInput *in : node->inputs) {
    if (in->link) {
      if (done.find(in->link->parent) == done.end()) {
        return false;
      }
    }
  }
  return true;
}

} /* namespace */

/* Sockets */

void ShaderInput::disconnect()
{
  if (link) {
    link->links.erase(remove(link->links.begin(), link->links.end(), this), link->links.end());
  }
  link = nullptr;
}

void ShaderOutput::disconnect()
{
  for (ShaderInput *sock : links) {
    sock->link = nullptr;
  }

  links.clear();
}

/* Node */

ShaderNode::ShaderNode(const NodeType *type) : Node(type)
{
  create_inputs_outputs(type);
}

ShaderNode::ShaderNode(const ShaderNode &other)
    : Node(other.type), bump(other.bump), special_type(other.special_type)
{
  /* Inputs and outputs are recreated, no links to other nodes will remain. */
  name = other.name;
  create_inputs_outputs(type);
}

void ShaderNode::create_inputs_outputs(const NodeType *type)
{
  for (const SocketType &socket : type->inputs) {
    if (socket.flags & SocketType::LINKABLE) {
      inputs.push_back(make_unique<ShaderInput>(socket, this));
    }
  }

  for (const SocketType &socket : type->outputs) {
    outputs.push_back(make_unique<ShaderOutput>(socket, this));
  }
}

ShaderInput *ShaderNode::input(const char *name)
{
  for (ShaderInput *socket : inputs) {
    if (socket->name() == name) {
      return socket;
    }
  }

  return nullptr;
}

ShaderOutput *ShaderNode::output(const char *name)
{
  for (ShaderOutput *socket : outputs) {
    if (socket->name() == name) {
      return socket;
    }
  }

  return nullptr;
}

ShaderInput *ShaderNode::input(ustring name)
{
  for (ShaderInput *socket : inputs) {
    if (socket->name() == name) {
      return socket;
    }
  }

  return nullptr;
}

ShaderOutput *ShaderNode::output(ustring name)
{
  for (ShaderOutput *socket : outputs) {
    if (socket->name() == name) {
      return socket;
    }
  }

  return nullptr;
}

void ShaderNode::disconnect_unused_input(const char *name)
{
  ShaderInput *socket = input(name);
  if (socket && socket->link) {
    socket->disconnect();
  }
}

void ShaderNode::remove_input(ShaderInput *input)
{
  assert(input->link == nullptr);
  inputs.erase(input);
}

void ShaderNode::attributes(Shader *shader, AttributeRequestSet *attributes)
{
  for (ShaderInput *input : inputs) {
    if (!input->link) {
      if (input->flags() & SocketType::LINK_TEXTURE_GENERATED) {
        if (shader->has_surface_link()) {
          attributes->add(ATTR_STD_GENERATED);
        }
        if (shader->has_volume) {
          attributes->add(ATTR_STD_GENERATED_TRANSFORM);
        }
      }
      else if (input->flags() & SocketType::LINK_TEXTURE_UV) {
        if (shader->has_surface_link()) {
          attributes->add(ATTR_STD_UV);
        }
      }
    }
  }
}

bool ShaderNode::equals(const ShaderNode &other)
{
  if (type != other.type || bump != other.bump) {
    return false;
  }

  assert(inputs.size() == other.inputs.size());

  /* Compare unlinkable sockets */
  for (const SocketType &socket : type->inputs) {
    if (!(socket.flags & SocketType::LINKABLE)) {
      if (!Node::equals_value(other, socket)) {
        return false;
      }
    }
  }

  /* Compare linkable input sockets */
  for (int i = 0; i < inputs.size(); ++i) {
    ShaderInput *input_a = inputs[i];
    ShaderInput *input_b = other.inputs[i];
    if (input_a->link == nullptr && input_b->link == nullptr) {
      /* Unconnected inputs are expected to have the same value. */
      if (!Node::equals_value(other, input_a->socket_type)) {
        return false;
      }
    }
    else if (input_a->link != nullptr && input_b->link != nullptr) {
      /* Expect links are to come from the same exact socket. */
      if (input_a->link != input_b->link) {
        return false;
      }
    }
    else {
      /* One socket has a link and another has not, inputs can't be
       * considered equal.
       */
      return false;
    }
  }

  return true;
}

/* Graph */

ShaderGraph::ShaderGraph()
{
  finalized = false;
  simplified = false;
  num_node_ids = 0;
  create_node<OutputNode>();
}

ShaderGraph::~ShaderGraph()
{
  clear_nodes();
}

void ShaderGraph::add_node(unique_ptr<ShaderNode> &&node)
{
  assert(!finalized);
  simplified = false;

  node->set_owner(this);
  node->id = num_node_ids++;
  nodes.push_back(std::move(node));
}

OutputNode *ShaderGraph::output()
{
  return static_cast<OutputNode *>(nodes[0]);
}

void ShaderGraph::connect(ShaderOutput *from, ShaderInput *to)
{
  assert(!finalized);
  assert(from && to);

  if (to->link) {
    LOG_WARNING << "Graph connect: input already connected.";
    return;
  }

  if (from->type() != to->type()) {
    /* can't do automatic conversion from closure */
    if (from->type() == SocketType::CLOSURE) {
      LOG_WARNING << "Shader graph connect: can only connect closure to closure ("
                  << from->parent->name.c_str() << "." << from->name().c_str() << " to "
                  << to->parent->name.c_str() << "." << to->name().c_str() << ")";
      return;
    }

    /* add automatic conversion node in case of type mismatch */
    ShaderNode *convert;
    ShaderInput *convert_in;

    if (to->type() == SocketType::CLOSURE) {
      EmissionNode *emission = create_node<EmissionNode>();
      emission->from_auto_conversion = true;
      emission->set_color(one_float3());
      emission->set_strength(1.0f);
      convert = emission;
      /* Connect float inputs to Strength to save an additional Value->Color conversion. */
      if (from->type() == SocketType::FLOAT) {
        convert_in = convert->input("Strength");
      }
      else {
        convert_in = convert->input("Color");
      }
    }
    else {
      convert = create_node<ConvertNode>(from->type(), to->type(), true);
      convert_in = convert->inputs[0];
    }

    connect(from, convert_in);
    connect(convert->outputs[0], to);
  }
  else {
    /* types match, just connect */
    to->link = from;
    from->links.push_back(to);
  }
}

void ShaderGraph::disconnect(ShaderOutput *from)
{
  assert(!finalized);
  simplified = false;

  from->disconnect();
}

void ShaderGraph::disconnect(ShaderInput *to)
{
  assert(!finalized);
  assert(to->link);
  simplified = false;

  to->disconnect();
}

void ShaderGraph::relink(ShaderInput *from, ShaderInput *to)
{
  ShaderOutput *out = from->link;
  if (out) {
    disconnect(from);
    connect(out, to);
  }
  to->parent->copy_value(to->socket_type, *(from->parent), from->socket_type);
}

void ShaderGraph::relink(ShaderOutput *from, ShaderOutput *to)
{
  /* Copy because disconnect modifies this list. */
  const vector<ShaderInput *> outputs = from->links;

  for (ShaderInput *sock : outputs) {
    disconnect(sock);
    if (to) {
      connect(to, sock);
    }
  }
}

void ShaderGraph::relink(ShaderNode *node, ShaderOutput *from, ShaderOutput *to)
{
  simplified = false;

  /* Copy because disconnect modifies this list */
  const vector<ShaderInput *> outputs = from->links;

  /* Bypass node by moving all links from "from" to "to" */
  for (ShaderInput *sock : node->inputs) {
    if (sock->link) {
      disconnect(sock);
    }
  }

  for (ShaderInput *sock : outputs) {
    disconnect(sock);
    if (to) {
      connect(to, sock);
    }
  }
}

void ShaderGraph::simplify(Scene *scene)
{
  if (!simplified) {
    expand();
    default_inputs(scene->shader_manager->use_osl());
    clean(scene);
    refine_bump_nodes();

    simplified = true;
  }
}

void ShaderGraph::finalize(Scene *scene, bool do_bump, bool bump_in_object_space)
{
  /* before compiling, the shader graph may undergo a number of modifications.
   * currently we set default geometry shader inputs, and create automatic bump
   * from displacement. a graph can be finalized only once, and should not be
   * modified afterwards. */

  if (!finalized) {
    simplify(scene);

    if (do_bump) {
      bump_from_displacement(bump_in_object_space);
    }

    ShaderInput *surface_in = output()->input("Surface");
    ShaderInput *volume_in = output()->input("Volume");

    /* todo: make this work when surface and volume closures are tangled up */

    if (surface_in->link) {
      transform_multi_closure(surface_in->link->parent, nullptr, false);
    }
    if (volume_in->link) {
      transform_multi_closure(volume_in->link->parent, nullptr, true);
    }

    finalized = true;
  }
}

void ShaderGraph::find_dependencies(ShaderNodeSet &dependencies, ShaderInput *input)
{
  /* find all nodes that this input depends on directly and indirectly */
  ShaderNode *node = (input->link) ? input->link->parent : nullptr;

  if (node != nullptr && dependencies.find(node) == dependencies.end()) {
    for (ShaderInput *in : node->inputs) {
      find_dependencies(dependencies, in);
    }

    dependencies.insert(node);
  }
}

void ShaderGraph::clear_nodes()
{
  nodes.clear();
}

void ShaderGraph::copy_nodes(ShaderNodeSet &nodes, ShaderNodeMap &nnodemap)
{
  /* copy a set of nodes, and the links between them. the assumption is
   * made that all nodes that inputs are linked to are in the set too. */

  /* copy nodes */
  for (ShaderNode *node : nodes) {
    ShaderNode *nnode = node->clone(this);
    nnodemap[node] = nnode;
  }

  /* recreate links */
  for (ShaderNode *node : nodes) {
    for (ShaderInput *input : node->inputs) {
      if (input->link) {
        /* find new input and output */
        ShaderNode *nfrom = nnodemap[input->link->parent];
        ShaderNode *nto = nnodemap[input->parent];
        ShaderOutput *noutput = nfrom->output(input->link->name());
        ShaderInput *ninput = nto->input(input->name());

        /* connect */
        connect(noutput, ninput);
      }
    }
  }
}

/* Graph simplification */
/* ******************** */

/* Remove proxy nodes.
 *
 * These only exists temporarily when exporting groups, and we must remove them
 * early so that node->attributes() and default links do not see them.
 */
void ShaderGraph::remove_proxy_nodes()
{
  vector<bool> removed(num_node_ids, false);
  bool any_node_removed = false;

  for (ShaderNode *node : nodes) {
    if (node->special_type == SHADER_SPECIAL_TYPE_PROXY) {
      ConvertNode *proxy = static_cast<ConvertNode *>(node);
      ShaderInput *input = proxy->inputs[0];
      ShaderOutput *output = proxy->outputs[0];

      /* bypass the proxy node */
      if (input->link) {
        relink(proxy, output, input->link);
      }
      else {
        /* Copy because disconnect modifies this list */
        const vector<ShaderInput *> links(output->links);

        for (ShaderInput *to : links) {
          /* Remove any auto-convert nodes too if they lead to
           * sockets with an automatically set default value. */
          ShaderNode *tonode = to->parent;

          if (tonode->special_type == SHADER_SPECIAL_TYPE_AUTOCONVERT) {
            bool all_links_removed = true;
            const vector<ShaderInput *> links = tonode->outputs[0]->links;

            for (ShaderInput *autoin : links) {
              if (autoin->flags() & SocketType::DEFAULT_LINK_MASK) {
                disconnect(autoin);
              }
              else {
                all_links_removed = false;
              }
            }

            if (all_links_removed) {
              removed[tonode->id] = true;
            }
          }

          disconnect(to);

          /* transfer the default input value to the target socket */
          tonode->copy_value(to->socket_type, *proxy, input->socket_type);
        }
      }

      removed[proxy->id] = true;
      any_node_removed = true;
    }
  }

  /* remove nodes */
  if (any_node_removed) {
    unique_ptr_vector<ShaderNode> newnodes;

    for (size_t i = 0; i < nodes.size(); i++) {
      unique_ptr<ShaderNode> node = nodes.steal(i);
      if (!removed[node->id]) {
        newnodes.push_back(std::move(node));
      }
    }

    nodes = std::move(newnodes);
  }
}

/* Constant folding.
 *
 * Try to constant fold some nodes, and pipe result directly to
 * the input socket of connected nodes.
 */
void ShaderGraph::constant_fold(Scene *scene)
{
  ShaderNodeSet done;
  ShaderNodeSet scheduled;
  queue<ShaderNode *> traverse_queue;

  const bool has_displacement = (output()->input("Displacement")->link != nullptr);

  /* Schedule nodes which doesn't have any dependencies. */
  for (ShaderNode *node : nodes) {
    if (!check_node_inputs_has_links(node)) {
      traverse_queue.push(node);
      scheduled.insert(node);
    }
  }

  while (!traverse_queue.empty()) {
    ShaderNode *node = traverse_queue.front();
    traverse_queue.pop();
    done.insert(node);
    for (ShaderOutput *output : node->outputs) {
      if (output->links.empty()) {
        continue;
      }
      /* Schedule node which was depending on the value,
       * when possible. Do it before disconnect.
       */
      for (ShaderInput *input : output->links) {
        if (scheduled.find(input->parent) != scheduled.end()) {
          /* Node might not be optimized yet but scheduled already
           * by other dependencies. No need to re-schedule it.
           */
          continue;
        }
        /* Schedule node if its inputs are fully done. */
        if (check_node_inputs_traversed(input->parent, done)) {
          traverse_queue.push(input->parent);
          scheduled.insert(input->parent);
        }
      }
      /* Optimize current node. */
      const ConstantFolder folder(this, node, output, scene);
      node->constant_fold(folder);
    }
  }

  /* Folding might have removed all nodes connected to the displacement output
   * even tho there is displacement to be applied, so add in a value node if
   * that happens to ensure there is still a valid graph for displacement.
   */
  if (has_displacement && !output()->input("Displacement")->link) {
    ColorNode *value = create_node<ColorNode>();
    value->set_value(output()->get_displacement());

    connect(value->output("Color"), output()->input("Displacement"));
  }
}

/* Simplification. */
void ShaderGraph::simplify_settings(Scene *scene)
{
  for (ShaderNode *node : nodes) {
    node->simplify_settings(scene);
  }
}

/* Deduplicate nodes with same settings. */
void ShaderGraph::deduplicate_nodes()
{
  /* NOTES:
   * - Deduplication happens for nodes which has same exact settings and same
   *   exact input links configuration (either connected to same output or has
   *   the same exact default value).
   * - Deduplication happens in the bottom-top manner, so we know for fact that
   *   all traversed nodes are either can not be deduplicated at all or were
   *   already deduplicated.
   */

  ShaderNodeSet scheduled;
  ShaderNodeSet done;
  map<ustring, ShaderNodeSet> candidates;
  queue<ShaderNode *> traverse_queue;
  int num_deduplicated = 0;

  /* Schedule nodes which doesn't have any dependencies. */
  for (ShaderNode *node : nodes) {
    if (!check_node_inputs_has_links(node)) {
      traverse_queue.push(node);
      scheduled.insert(node);
    }
  }

  while (!traverse_queue.empty()) {
    ShaderNode *node = traverse_queue.front();
    traverse_queue.pop();
    done.insert(node);
    /* Schedule the nodes which were depending on the current node. */
    bool has_output_links = false;
    for (ShaderOutput *output : node->outputs) {
      for (ShaderInput *input : output->links) {
        has_output_links = true;
        if (scheduled.find(input->parent) != scheduled.end()) {
          /* Node might not be optimized yet but scheduled already
           * by other dependencies. No need to re-schedule it.
           */
          continue;
        }
        /* Schedule node if its inputs are fully done. */
        if (check_node_inputs_traversed(input->parent, done)) {
          traverse_queue.push(input->parent);
          scheduled.insert(input->parent);
        }
      }
    }
    /* Only need to care about nodes that are actually used */
    if (!has_output_links) {
      continue;
    }
    /* Try to merge this node with another one. */
    ShaderNode *merge_with = nullptr;
    for (ShaderNode *other_node : candidates[node->type->name]) {
      if (node != other_node && node->equals(*other_node)) {
        merge_with = other_node;
        break;
      }
    }
    /* If found an equivalent, merge; otherwise keep node for later merges */
    if (merge_with != nullptr) {
      for (int i = 0; i < node->outputs.size(); ++i) {
        relink(node, node->outputs[i], merge_with->outputs[i]);
      }
      num_deduplicated++;
    }
    else {
      candidates[node->type->name].insert(node);
    }
  }

  if (num_deduplicated > 0) {
    LOG_DEBUG << "Deduplicated " << num_deduplicated << " nodes.";
  }
}

/* Does two optimizations:
 * - Check whether volume output has meaningful nodes, otherwise disconnect the output.
 * - Tag volume attribute nodes as supporting stochastic sampling. */
void ShaderGraph::optimize_volume_output()
{
  ShaderInput *volume_in = output()->input("Volume");
  if (volume_in->link == nullptr) {
    return;
  }

  bool has_valid_volume = false;

  using ShaderNodeAndNonLinear = std::pair<ShaderNode *, bool>;
  set<ShaderNodeAndNonLinear, ShaderNodeIDAndBoolComparator> scheduled;
  queue<ShaderNodeAndNonLinear> traverse_queue;

  /* Schedule volume output. */
  traverse_queue.emplace(volume_in->link->parent, false);
  scheduled.insert({volume_in->link->parent, false});

  /* Traverse down the tree. */
  while (!traverse_queue.empty()) {
    auto [node, nonlinear] = traverse_queue.front();
    traverse_queue.pop();

    /* Disable stochastic sampling on node if its contribution is nonlinear.
     * This defaults to true in the class, so we only need to disable it. */
    if (nonlinear && node->type == AttributeNode::get_node_type()) {
      static_cast<AttributeNode *>(node)->stochastic_sample = false;
    }
    nonlinear = nonlinear || !node->is_linear_operation();

    /* Node is fully valid for volume, won't be able to optimize it out. */
    if (node->has_volume_support()) {
      has_valid_volume = true;
    }

    for (ShaderInput *input : node->inputs) {
      if (input->link == nullptr) {
        continue;
      }
      ShaderNode *input_node = input->link->parent;
      if (scheduled.find({input_node, nonlinear}) != scheduled.end()) {
        continue;
      }
      traverse_queue.emplace(input_node, nonlinear);
      scheduled.insert({input_node, nonlinear});
    }
  }

  if (LOG_IS_ON(LOG_LEVEL_DEBUG)) {
    for (ShaderNode *node : nodes) {
      if (node->type == AttributeNode::get_node_type() &&
          static_cast<AttributeNode *>(node)->stochastic_sample)
      {
        LOG_DEBUG << "Volume attribute node " << node->name << " uses stochastic sampling";
      }
    }
  }

  if (!has_valid_volume) {
    /* We can remove the entire volume shader. */
    LOG_DEBUG << "Disconnect meaningless volume output.";
    disconnect(volume_in->link);
  }
}

void ShaderGraph::break_cycles(ShaderNode *node, vector<bool> &visited, vector<bool> &on_stack)
{
  visited[node->id] = true;
  on_stack[node->id] = true;

  for (ShaderInput *input : node->inputs) {
    if (input->link) {
      ShaderNode *depnode = input->link->parent;

      if (on_stack[depnode->id]) {
        /* break cycle */
        disconnect(input);
        LOG_WARNING << "Shader graph: detected cycle in graph, connection removed.";
      }
      else if (!visited[depnode->id]) {
        /* visit dependencies */
        break_cycles(depnode, visited, on_stack);
      }
    }
  }

  on_stack[node->id] = false;
}

void ShaderGraph::compute_displacement_hash()
{
  /* Compute hash of all nodes linked to displacement, to detect if we need
   * to recompute displacement when shader nodes change. */
  ShaderInput *displacement_in = output()->input("Displacement");

  if (!displacement_in->link) {
    displacement_hash = "";
    return;
  }

  ShaderNodeSet nodes_displace;
  find_dependencies(nodes_displace, displacement_in);

  MD5Hash md5;
  for (ShaderNode *node : nodes_displace) {
    node->hash(md5);
    for (ShaderInput *input : node->inputs) {
      int link_id = (input->link) ? input->link->parent->id : 0;
      md5.append((uint8_t *)&link_id, sizeof(link_id));
      md5.append((input->link) ? input->link->name().c_str() : "");
    }

    if (node->special_type == SHADER_SPECIAL_TYPE_OSL) {
      /* Hash takes into account socket values, to detect changes
       * in the code of the node we need an exception. */
      OSLNode *oslnode = static_cast<OSLNode *>(node);
      md5.append(oslnode->bytecode_hash);
    }
  }

  displacement_hash = md5.get_hex();
}

void ShaderGraph::clean(Scene *scene)
{
  /* Graph simplification */

  /* NOTE: Remove proxy nodes was already done. */
  constant_fold(scene);
  simplify_settings(scene);
  deduplicate_nodes();
  optimize_volume_output();

  /* we do two things here: find cycles and break them, and remove unused
   * nodes that don't feed into the output. how cycles are broken is
   * undefined, they are invalid input, the important thing is to not crash */

  vector<bool> visited(num_node_ids, false);
  vector<bool> on_stack(num_node_ids, false);

  /* break cycles */
  break_cycles(output(), visited, on_stack);
  for (ShaderNode *node : nodes) {
    if (node->special_type == SHADER_SPECIAL_TYPE_OUTPUT_AOV) {
      break_cycles(node, visited, on_stack);
    }
  }

  /* disconnect unused nodes */
  for (ShaderNode *node : nodes) {
    if (!visited[node->id]) {
      for (ShaderInput *to : node->inputs) {
        ShaderOutput *from = to->link;

        if (from) {
          to->link = nullptr;
          from->links.erase(remove(from->links.begin(), from->links.end(), to), from->links.end());
        }
      }
    }
  }

  /* remove unused nodes */
  unique_ptr_vector<ShaderNode> newnodes;

  for (size_t i = 0; i < nodes.size(); i++) {
    unique_ptr<ShaderNode> node = nodes.steal(i);
    if (visited[node->id]) {
      newnodes.push_back(std::move(node));
    }
  }

  nodes = std::move(newnodes);
}

void ShaderGraph::expand()
{
  /* Call expand on all nodes, to generate additional nodes.
   * No range based for loop because we modify the vector, and want to expand
   * newly generated nodes too. */
  for (size_t i = 0; i < nodes.size(); i++) {
    ShaderNode *node = nodes[i];
    node->expand(this);
  }
}

void ShaderGraph::default_inputs(bool do_osl)
{
  /* nodes can specify default texture coordinates, for now we give
   * everything the position by default, except for the sky texture */

  GeometryNode *geom = nullptr;
  TextureCoordinateNode *texco = nullptr;
  VectorTransformNode *normal_transform = nullptr;

  for (size_t i = 0; i < nodes.size(); i++) {
    ShaderNode *node = nodes[i];

    for (ShaderInput *input : node->inputs) {
      if (!input->link && (!(input->flags() & SocketType::OSL_INTERNAL) || do_osl)) {
        if (input->flags() & SocketType::LINK_TEXTURE_GENERATED) {
          if (!texco) {
            texco = create_node<TextureCoordinateNode>();
          }

          connect(texco->output("Generated"), input);
        }
        if (input->flags() & SocketType::LINK_TEXTURE_NORMAL) {
          if (!texco) {
            texco = create_node<TextureCoordinateNode>();
          }

          connect(texco->output("Normal"), input);
        }
        else if (input->flags() & SocketType::LINK_TEXTURE_UV) {
          if (!texco) {
            texco = create_node<TextureCoordinateNode>();
          }

          connect(texco->output("UV"), input);
        }
        else if (input->flags() & SocketType::LINK_TEXTURE_INCOMING) {
          if (!geom) {
            geom = create_node<GeometryNode>();
          }
          if (!normal_transform) {
            normal_transform = create_node<VectorTransformNode>();
            normal_transform->set_transform_type(NODE_VECTOR_TRANSFORM_TYPE_NORMAL);
            normal_transform->set_convert_from(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_WORLD);
            normal_transform->set_convert_to(NODE_VECTOR_TRANSFORM_CONVERT_SPACE_OBJECT);
            connect(geom->output("Incoming"), normal_transform->input("Vector"));
          }

          connect(normal_transform->output("Vector"), input);
        }
        else if (input->flags() & SocketType::LINK_INCOMING) {
          if (!geom) {
            geom = create_node<GeometryNode>();
          }

          connect(geom->output("Incoming"), input);
        }
        else if (input->flags() & SocketType::LINK_NORMAL) {
          if (!geom) {
            geom = create_node<GeometryNode>();
          }

          connect(geom->output("Normal"), input);
        }
        else if (input->flags() & SocketType::LINK_POSITION) {
          if (!geom) {
            geom = create_node<GeometryNode>();
          }

          connect(geom->output("Position"), input);
        }
        else if (input->flags() & SocketType::LINK_TANGENT) {
          if (!geom) {
            geom = create_node<GeometryNode>();
          }

          connect(geom->output("Tangent"), input);
        }
      }
    }
  }
}

void ShaderGraph::refine_bump_nodes()
{
  /* We transverse the node graph looking for bump nodes, when we find them,
   * like in bump_from_displacement(), we copy the sub-graph defined from "bump"
   * input to the inputs "center","dx" and "dy" What is in "bump" input is moved
   * to "center" input. */

  /* No range based for loop because we modify the vector. */
  for (int i = 0; i < nodes.size(); i++) {
    ShaderNode *node = nodes[i];

    if (node->special_type == SHADER_SPECIAL_TYPE_BUMP && node->input("Height")->link) {
      BumpNode *bump = static_cast<BumpNode *>(node);
      ShaderInput *bump_input = node->input("Height");
      ShaderNodeSet nodes_bump;

      /* Make 2 extra copies of the subgraph defined in Bump input. */
      ShaderNodeMap nodes_dx;
      ShaderNodeMap nodes_dy;

      /* Find dependencies for the given input. */
      find_dependencies(nodes_bump, bump_input);

      copy_nodes(nodes_bump, nodes_dx);
      copy_nodes(nodes_bump, nodes_dy);

      /* Mark nodes to indicate they are use for bump computation, so
       * that any texture coordinates are shifted by dx/dy when sampling. */
      for (ShaderNode *node : nodes_bump) {
        node->bump = SHADER_BUMP_CENTER;
        node->bump_filter_width = bump->get_filter_width();
      }
      for (const NodePair &pair : nodes_dx) {
        pair.second->bump = SHADER_BUMP_DX;
        pair.second->bump_filter_width = bump->get_filter_width();
      }
      for (const NodePair &pair : nodes_dy) {
        pair.second->bump = SHADER_BUMP_DY;
        pair.second->bump_filter_width = bump->get_filter_width();
      }

      ShaderOutput *out = bump_input->link;
      ShaderOutput *out_dx = nodes_dx[out->parent]->output(out->name());
      ShaderOutput *out_dy = nodes_dy[out->parent]->output(out->name());

      connect(out_dx, node->input("SampleX"));
      connect(out_dy, node->input("SampleY"));

      /* Connect what is connected is bump to sample-center input. */
      connect(out, node->input("SampleCenter"));

      /* Bump input is just for connectivity purpose for the graph input,
       * we re-connected this input to sample-center, so lets disconnect it
       * from bump input. */
      disconnect(bump_input);
    }
  }
}

void ShaderGraph::bump_from_displacement(bool use_object_space)
{
  /* generate bump mapping automatically from displacement. bump mapping is
   * done using a 3-tap filter, computing the displacement at the center,
   * and two other positions shifted by ray differentials.
   *
   * since the input to displacement is a node graph, we need to ensure that
   * all texture coordinates use are shift by the ray differentials. for this
   * reason we make 3 copies of the node subgraph defining the displacement,
   * with each different geometry and texture coordinate nodes that generate
   * different shifted coordinates.
   *
   * these 3 displacement values are then fed into the bump node, which will
   * output the perturbed normal. */

  ShaderInput *displacement_in = output()->input("Displacement");

  if (!displacement_in->link) {
    return;
  }

  /* find dependencies for the given input */
  ShaderNodeSet nodes_displace;
  find_dependencies(nodes_displace, displacement_in);

  /* Add bump node. */
  BumpNode *bump = create_node<BumpNode>();
  bump->set_use_object_space(use_object_space);
  bump->set_distance(1.0f);

  /* copy nodes for 3 bump samples */
  ShaderNodeMap nodes_center;
  ShaderNodeMap nodes_dx;
  ShaderNodeMap nodes_dy;

  copy_nodes(nodes_displace, nodes_center);
  copy_nodes(nodes_displace, nodes_dx);
  copy_nodes(nodes_displace, nodes_dy);

  /* mark nodes to indicate they are use for bump computation, so
   * that any texture coordinates are shifted by dx/dy when sampling */
  for (const NodePair &pair : nodes_center) {
    pair.second->bump = SHADER_BUMP_CENTER;
    pair.second->bump_filter_width = bump->get_filter_width();
  }
  for (const NodePair &pair : nodes_dx) {
    pair.second->bump = SHADER_BUMP_DX;
    pair.second->bump_filter_width = bump->get_filter_width();
  }
  for (const NodePair &pair : nodes_dy) {
    pair.second->bump = SHADER_BUMP_DY;
    pair.second->bump_filter_width = bump->get_filter_width();
  }

  /* add set normal node and connect the bump normal output to the set normal
   * output, so it can finally set the shader normal, note we are only doing
   * this for bump from displacement, this will be the only bump allowed to
   * overwrite the shader normal */
  ShaderNode *set_normal = create_node<SetNormalNode>();

  /* Connect copied graphs to bump node. */
  ShaderOutput *out = displacement_in->link;
  ShaderOutput *out_center = nodes_center[out->parent]->output(out->name());
  ShaderOutput *out_dx = nodes_dx[out->parent]->output(out->name());
  ShaderOutput *out_dy = nodes_dy[out->parent]->output(out->name());

  /* convert displacement vector to height */
  VectorMathNode *dot_center = create_node<VectorMathNode>();
  VectorMathNode *dot_dx = create_node<VectorMathNode>();
  VectorMathNode *dot_dy = create_node<VectorMathNode>();

  dot_center->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
  dot_dx->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);
  dot_dy->set_math_type(NODE_VECTOR_MATH_DOT_PRODUCT);

  GeometryNode *geom = create_node<GeometryNode>();
  connect(geom->output("Normal"), bump->input("Normal"));
  connect(geom->output("Normal"), dot_center->input("Vector2"));
  connect(geom->output("Normal"), dot_dx->input("Vector2"));
  connect(geom->output("Normal"), dot_dy->input("Vector2"));

  connect(out_center, dot_center->input("Vector1"));
  connect(out_dx, dot_dx->input("Vector1"));
  connect(out_dy, dot_dy->input("Vector1"));

  connect(dot_center->output("Value"), bump->input("SampleCenter"));
  connect(dot_dx->output("Value"), bump->input("SampleX"));
  connect(dot_dy->output("Value"), bump->input("SampleY"));

  /* connect the bump out to the set normal in: */
  connect(bump->output("Normal"), set_normal->input("Direction"));

  /* connect to output node */
  connect(set_normal->output("Normal"), output()->input("Normal"));
}

void ShaderGraph::transform_multi_closure(ShaderNode *node, ShaderOutput *weight_out, bool volume)
{
  /* for SVM in multi closure mode, this transforms the shader mix/add part of
   * the graph into nodes that feed weights into closure nodes. this is too
   * avoid building a closure tree and then flattening it, and instead write it
   * directly to an array */

  if (node->special_type == SHADER_SPECIAL_TYPE_COMBINE_CLOSURE) {
    ShaderInput *fin = node->input("Fac");
    ShaderInput *cl1in = node->input("Closure1");
    ShaderInput *cl2in = node->input("Closure2");
    ShaderOutput *weight1_out;
    ShaderOutput *weight2_out;

    if (fin) {
      /* mix closure: add node to mix closure weights */
      MixClosureWeightNode *mix_node = create_node<MixClosureWeightNode>();
      ShaderInput *fac_in = mix_node->input("Fac");
      ShaderInput *weight_in = mix_node->input("Weight");

      if (fin->link) {
        connect(fin->link, fac_in);
      }
      else {
        mix_node->set_fac(node->get_float(fin->socket_type));
      }

      if (weight_out) {
        connect(weight_out, weight_in);
      }

      weight1_out = mix_node->output("Weight1");
      weight2_out = mix_node->output("Weight2");
    }
    else {
      /* add closure: just pass on any weights */
      weight1_out = weight_out;
      weight2_out = weight_out;
    }

    if (cl1in->link) {
      transform_multi_closure(cl1in->link->parent, weight1_out, volume);
    }
    if (cl2in->link) {
      transform_multi_closure(cl2in->link->parent, weight2_out, volume);
    }
  }
  else {
    ShaderInput *weight_in = node->input((volume) ? "VolumeMixWeight" : "SurfaceMixWeight");

    /* not a closure node? */
    if (!weight_in) {
      return;
    }

    /* already has a weight connected to it? add weights */
    const float weight_value = node->get_float(weight_in->socket_type);
    if (weight_in->link || weight_value != 0.0f) {
      MathNode *math_node = create_node<MathNode>();

      if (weight_in->link) {
        connect(weight_in->link, math_node->input("Value1"));
      }
      else {
        math_node->set_value1(weight_value);
      }

      if (weight_out) {
        connect(weight_out, math_node->input("Value2"));
      }
      else {
        math_node->set_value2(1.0f);
      }

      weight_out = math_node->output("Value");
      if (weight_in->link) {
        disconnect(weight_in);
      }
    }

    /* connected to closure mix weight */
    if (weight_out) {
      connect(weight_out, weight_in);
    }
    else {
      node->set(weight_in->socket_type, weight_value + 1.0f);
    }
  }
}

int ShaderGraph::get_num_closures()
{
  int num_closures = 0;
  for (ShaderNode *node : nodes) {
    const ClosureType closure_type = node->get_closure_type();
    if (closure_type == CLOSURE_NONE_ID) {
      continue;
    }
    if (CLOSURE_IS_BSSRDF(closure_type)) {
      num_closures += 3;
    }
    else if (CLOSURE_IS_BSDF_MULTISCATTER(closure_type)) {
      num_closures += 2;
    }
    else if (CLOSURE_IS_PRINCIPLED(closure_type)) {
      num_closures += 12;
    }
    else if (CLOSURE_IS_VOLUME(closure_type)) {
      /* TODO(sergey): Verify this is still needed, since we have special minimized volume
       * storage for the volume steps. */
      num_closures += MAX_VOLUME_STACK_SIZE;
    }
    else if (closure_type == CLOSURE_BSDF_PHYSICAL_CONDUCTOR ||
             closure_type == CLOSURE_BSDF_F82_CONDUCTOR ||
             closure_type == CLOSURE_BSDF_MICROFACET_BECKMANN_GLASS_ID ||
             closure_type == CLOSURE_BSDF_MICROFACET_GGX_GLASS_ID ||
             closure_type == CLOSURE_BSDF_HAIR_CHIANG_ID ||
             closure_type == CLOSURE_BSDF_HAIR_HUANG_ID)
    {
      num_closures += 2;
    }
    else {
      ++num_closures;
    }
  }
  return num_closures;
}

void ShaderGraph::dump_graph(const char *filename)
{
  FILE *fd = fopen(filename, "w");

  if (fd == nullptr) {
    LOG_ERROR << "Error opening file for dumping the graph: " << filename;
    return;
  }

  fprintf(fd, "digraph shader_graph {\n");
  fprintf(fd, "ranksep=1.5\n");
  fprintf(fd, "rankdir=LR\n");
  fprintf(fd, "splines=false\n");

  for (ShaderNode *node : nodes) {
    fprintf(fd, "// NODE: %p\n", node);
    fprintf(fd, "\"%p\" [shape=record,label=\"{", node);
    if (!node->inputs.empty()) {
      fprintf(fd, "{");
      for (ShaderInput *socket : node->inputs) {
        if (socket != node->inputs[0]) {
          fprintf(fd, "|");
        }
        fprintf(fd, "<IN_%p>%s", socket, socket->name().c_str());
      }
      fprintf(fd, "}|");
    }
    fprintf(fd, "%s", node->name.c_str());
    if (node->bump == SHADER_BUMP_CENTER) {
      fprintf(fd, " (bump:center)");
    }
    else if (node->bump == SHADER_BUMP_DX) {
      fprintf(fd, " (bump:dx)");
    }
    else if (node->bump == SHADER_BUMP_DY) {
      fprintf(fd, " (bump:dy)");
    }
    if (!node->outputs.empty()) {
      fprintf(fd, "|{");
      for (ShaderOutput *socket : node->outputs) {
        if (socket != node->outputs[0]) {
          fprintf(fd, "|");
        }
        fprintf(fd, "<OUT_%p>%s", socket, socket->name().c_str());
      }
      fprintf(fd, "}");
    }
    fprintf(fd, "}\"]");
  }

  for (ShaderNode *node : nodes) {
    for (ShaderOutput *output : node->outputs) {
      for (ShaderInput *input : output->links) {
        fprintf(fd,
                "// CONNECTION: OUT_%p->IN_%p (%s:%s)\n",
                output,
                input,
                output->name().c_str(),
                input->name().c_str());
        fprintf(fd,
                "\"%p\":\"OUT_%p\":e -> \"%p\":\"IN_%p\":w [label=\"\"]\n",
                output->parent,
                output,
                input->parent,
                input);
      }
    }
  }

  fprintf(fd, "}\n");
  fclose(fd);
}

CCL_NAMESPACE_END
