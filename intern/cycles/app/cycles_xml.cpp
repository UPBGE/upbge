/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2011-2022 Blender Foundation */

#include <stdio.h>

#include <algorithm>
#include <iterator>
#include <sstream>

#include "graph/node_xml.h"

#include "scene/alembic.h"
#include "scene/background.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/mesh.h"
#include "scene/object.h"
#include "scene/osl.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/shader_graph.h"
#include "scene/shader_nodes.h"

#include "subd/patch.h"
#include "subd/split.h"

#include "util/foreach.h"
#include "util/path.h"
#include "util/projection.h"
#include "util/transform.h"
#include "util/xml.h"

#include "app/cycles_xml.h"

CCL_NAMESPACE_BEGIN

/* XML reading state */

struct XMLReadState : public XMLReader {
  Scene *scene;      /* Scene pointer. */
  Transform tfm;     /* Current transform state. */
  bool smooth;       /* Smooth normal state. */
  Shader *shader;    /* Current shader. */
  string base;       /* Base path to current file. */
  float dicing_rate; /* Current dicing rate. */

  XMLReadState() : scene(NULL), smooth(false), shader(NULL), dicing_rate(1.0f)
  {
    tfm = transform_identity();
  }
};

/* Attribute Reading */

static bool xml_read_int(int *value, xml_node node, const char *name)
{
  xml_attribute attr = node.attribute(name);

  if (attr) {
    *value = atoi(attr.value());
    return true;
  }

  return false;
}

static bool xml_read_int_array(vector<int> &value, xml_node node, const char *name)
{
  xml_attribute attr = node.attribute(name);

  if (attr) {
    vector<string> tokens;
    string_split(tokens, attr.value());

    foreach (const string &token, tokens)
      value.push_back(atoi(token.c_str()));

    return true;
  }

  return false;
}

static bool xml_read_float(float *value, xml_node node, const char *name)
{
  xml_attribute attr = node.attribute(name);

  if (attr) {
    *value = (float)atof(attr.value());
    return true;
  }

  return false;
}

static bool xml_read_float_array(vector<float> &value, xml_node node, const char *name)
{
  xml_attribute attr = node.attribute(name);

  if (attr) {
    vector<string> tokens;
    string_split(tokens, attr.value());

    foreach (const string &token, tokens)
      value.push_back((float)atof(token.c_str()));

    return true;
  }

  return false;
}

static bool xml_read_float3(float3 *value, xml_node node, const char *name)
{
  vector<float> array;

  if (xml_read_float_array(array, node, name) && array.size() == 3) {
    *value = make_float3(array[0], array[1], array[2]);
    return true;
  }

  return false;
}

static bool xml_read_float3_array(vector<float3> &value, xml_node node, const char *name)
{
  vector<float> array;

  if (xml_read_float_array(array, node, name)) {
    for (size_t i = 0; i < array.size(); i += 3)
      value.push_back(make_float3(array[i + 0], array[i + 1], array[i + 2]));

    return true;
  }

  return false;
}

static bool xml_read_float4(float4 *value, xml_node node, const char *name)
{
  vector<float> array;

  if (xml_read_float_array(array, node, name) && array.size() == 4) {
    *value = make_float4(array[0], array[1], array[2], array[3]);
    return true;
  }

  return false;
}

static bool xml_read_string(string *str, xml_node node, const char *name)
{
  xml_attribute attr = node.attribute(name);

  if (attr) {
    *str = attr.value();
    return true;
  }

  return false;
}

static bool xml_equal_string(xml_node node, const char *name, const char *value)
{
  xml_attribute attr = node.attribute(name);

  if (attr)
    return string_iequals(attr.value(), value);

  return false;
}

/* Camera */

static void xml_read_camera(XMLReadState &state, xml_node node)
{
  Camera *cam = state.scene->camera;

  int width = -1, height = -1;
  xml_read_int(&width, node, "width");
  xml_read_int(&height, node, "height");

  cam->set_full_width(width);
  cam->set_full_height(height);

  xml_read_node(state, cam, node);

  cam->set_matrix(state.tfm);

  cam->need_flags_update = true;
  cam->update(state.scene);
}

/* Alembic */

#ifdef WITH_ALEMBIC
static void xml_read_alembic(XMLReadState &state, xml_node graph_node)
{
  AlembicProcedural *proc = state.scene->create_node<AlembicProcedural>();
  xml_read_node(state, proc, graph_node);

  for (xml_node node = graph_node.first_child(); node; node = node.next_sibling()) {
    if (string_iequals(node.name(), "object")) {
      string path;
      if (xml_read_string(&path, node, "path")) {
        ustring object_path(path, 0);
        AlembicObject *object = static_cast<AlembicObject *>(
            proc->get_or_create_object(object_path));

        array<Node *> used_shaders = object->get_used_shaders();
        used_shaders.push_back_slow(state.shader);
        object->set_used_shaders(used_shaders);
      }
    }
  }
}
#endif

/* Shader */

static void xml_read_shader_graph(XMLReadState &state, Shader *shader, xml_node graph_node)
{
  xml_read_node(state, shader, graph_node);

  ShaderGraph *graph = new ShaderGraph();

  /* local state, shader nodes can't link to nodes outside the shader graph */
  XMLReader graph_reader;
  graph_reader.node_map[ustring("output")] = graph->output();

  for (xml_node node = graph_node.first_child(); node; node = node.next_sibling()) {
    ustring node_name(node.name());

    if (node_name == "connect") {
      /* connect nodes */
      vector<string> from_tokens, to_tokens;

      string_split(from_tokens, node.attribute("from").value());
      string_split(to_tokens, node.attribute("to").value());

      if (from_tokens.size() == 2 && to_tokens.size() == 2) {
        ustring from_node_name(from_tokens[0]);
        ustring from_socket_name(from_tokens[1]);
        ustring to_node_name(to_tokens[0]);
        ustring to_socket_name(to_tokens[1]);

        /* find nodes and sockets */
        ShaderOutput *output = NULL;
        ShaderInput *input = NULL;

        if (graph_reader.node_map.find(from_node_name) != graph_reader.node_map.end()) {
          ShaderNode *fromnode = (ShaderNode *)graph_reader.node_map[from_node_name];

          foreach (ShaderOutput *out, fromnode->outputs)
            if (string_iequals(out->socket_type.name.string(), from_socket_name.string()))
              output = out;

          if (!output)
            fprintf(stderr,
                    "Unknown output socket name \"%s\" on \"%s\".\n",
                    from_node_name.c_str(),
                    from_socket_name.c_str());
        }
        else
          fprintf(stderr, "Unknown shader node name \"%s\".\n", from_node_name.c_str());

        if (graph_reader.node_map.find(to_node_name) != graph_reader.node_map.end()) {
          ShaderNode *tonode = (ShaderNode *)graph_reader.node_map[to_node_name];

          foreach (ShaderInput *in, tonode->inputs)
            if (string_iequals(in->socket_type.name.string(), to_socket_name.string()))
              input = in;

          if (!input)
            fprintf(stderr,
                    "Unknown input socket name \"%s\" on \"%s\".\n",
                    to_socket_name.c_str(),
                    to_node_name.c_str());
        }
        else
          fprintf(stderr, "Unknown shader node name \"%s\".\n", to_node_name.c_str());

        /* connect */
        if (output && input)
          graph->connect(output, input);
      }
      else
        fprintf(stderr, "Invalid from or to value for connect node.\n");

      continue;
    }

    ShaderNode *snode = NULL;

#ifdef WITH_OSL
    if (node_name == "osl_shader") {
      ShaderManager *manager = state.scene->shader_manager;

      if (manager->use_osl()) {
        std::string filepath;

        if (xml_read_string(&filepath, node, "src")) {
          if (path_is_relative(filepath)) {
            filepath = path_join(state.base, filepath);
          }

          snode = OSLShaderManager::osl_node(graph, manager, filepath, "");

          if (!snode) {
            fprintf(stderr, "Failed to create OSL node from \"%s\".\n", filepath.c_str());
            continue;
          }
        }
        else {
          fprintf(stderr, "OSL node missing \"src\" attribute.\n");
          continue;
        }
      }
      else {
        fprintf(stderr, "OSL node without using --shadingsys osl.\n");
        continue;
      }
    }
    else
#endif
    {
      /* exception for name collision */
      if (node_name == "background")
        node_name = "background_shader";

      const NodeType *node_type = NodeType::find(node_name);

      if (!node_type) {
        fprintf(stderr, "Unknown shader node \"%s\".\n", node.name());
        continue;
      }
      else if (node_type->type != NodeType::SHADER) {
        fprintf(stderr, "Node type \"%s\" is not a shader node.\n", node_type->name.c_str());
        continue;
      }
      else if (node_type->create == NULL) {
        fprintf(stderr, "Can't create abstract node type \"%s\".\n", node_type->name.c_str());
        continue;
      }

      snode = (ShaderNode *)node_type->create(node_type);
      snode->set_owner(graph);
    }

    xml_read_node(graph_reader, snode, node);

    if (node_name == "image_texture") {
      ImageTextureNode *img = (ImageTextureNode *)snode;
      ustring filename(path_join(state.base, img->get_filename().string()));
      img->set_filename(filename);
    }
    else if (node_name == "environment_texture") {
      EnvironmentTextureNode *env = (EnvironmentTextureNode *)snode;
      ustring filename(path_join(state.base, env->get_filename().string()));
      env->set_filename(filename);
    }

    if (snode) {
      /* add to graph */
      graph->add(snode);
    }
  }

  shader->set_graph(graph);
  shader->tag_update(state.scene);
}

static void xml_read_shader(XMLReadState &state, xml_node node)
{
  Shader *shader = new Shader();
  xml_read_shader_graph(state, shader, node);
  state.scene->shaders.push_back(shader);
}

/* Background */

static void xml_read_background(XMLReadState &state, xml_node node)
{
  /* Background Settings */
  xml_read_node(state, state.scene->background, node);

  /* Background Shader */
  Shader *shader = state.scene->default_background;
  xml_read_shader_graph(state, shader, node);
}

/* Mesh */

static Mesh *xml_add_mesh(Scene *scene, const Transform &tfm)
{
  /* create mesh */
  Mesh *mesh = new Mesh();
  scene->geometry.push_back(mesh);

  /* Create object. */
  Object *object = new Object();
  object->set_geometry(mesh);
  object->set_tfm(tfm);
  scene->objects.push_back(object);

  return mesh;
}

static void xml_read_mesh(const XMLReadState &state, xml_node node)
{
  /* add mesh */
  Mesh *mesh = xml_add_mesh(state.scene, state.tfm);
  array<Node *> used_shaders = mesh->get_used_shaders();
  used_shaders.push_back_slow(state.shader);
  mesh->set_used_shaders(used_shaders);

  /* read state */
  int shader = 0;
  bool smooth = state.smooth;

  /* read vertices and polygons */
  vector<float3> P;
  vector<float> UV;
  vector<int> verts, nverts;

  xml_read_float3_array(P, node, "P");
  xml_read_int_array(verts, node, "verts");
  xml_read_int_array(nverts, node, "nverts");

  if (xml_equal_string(node, "subdivision", "catmull-clark")) {
    mesh->set_subdivision_type(Mesh::SUBDIVISION_CATMULL_CLARK);
  }
  else if (xml_equal_string(node, "subdivision", "linear")) {
    mesh->set_subdivision_type(Mesh::SUBDIVISION_LINEAR);
  }

  array<float3> P_array;
  P_array = P;

  if (mesh->get_subdivision_type() == Mesh::SUBDIVISION_NONE) {
    /* create vertices */

    mesh->set_verts(P_array);

    size_t num_triangles = 0;
    for (size_t i = 0; i < nverts.size(); i++)
      num_triangles += nverts[i] - 2;
    mesh->reserve_mesh(mesh->get_verts().size(), num_triangles);

    /* create triangles */
    int index_offset = 0;

    for (size_t i = 0; i < nverts.size(); i++) {
      for (int j = 0; j < nverts[i] - 2; j++) {
        int v0 = verts[index_offset];
        int v1 = verts[index_offset + j + 1];
        int v2 = verts[index_offset + j + 2];

        assert(v0 < (int)P.size());
        assert(v1 < (int)P.size());
        assert(v2 < (int)P.size());

        mesh->add_triangle(v0, v1, v2, shader, smooth);
      }

      index_offset += nverts[i];
    }

    if (xml_read_float_array(UV, node, "UV")) {
      ustring name = ustring("UVMap");
      Attribute *attr = mesh->attributes.add(ATTR_STD_UV, name);
      float2 *fdata = attr->data_float2();

      /* loop over the triangles */
      index_offset = 0;
      for (size_t i = 0; i < nverts.size(); i++) {
        for (int j = 0; j < nverts[i] - 2; j++) {
          int v0 = index_offset;
          int v1 = index_offset + j + 1;
          int v2 = index_offset + j + 2;

          assert(v0 * 2 + 1 < (int)UV.size());
          assert(v1 * 2 + 1 < (int)UV.size());
          assert(v2 * 2 + 1 < (int)UV.size());

          fdata[0] = make_float2(UV[v0 * 2], UV[v0 * 2 + 1]);
          fdata[1] = make_float2(UV[v1 * 2], UV[v1 * 2 + 1]);
          fdata[2] = make_float2(UV[v2 * 2], UV[v2 * 2 + 1]);
          fdata += 3;
        }

        index_offset += nverts[i];
      }
    }
  }
  else {
    /* create vertices */
    mesh->set_verts(P_array);

    size_t num_ngons = 0;
    size_t num_corners = 0;
    for (size_t i = 0; i < nverts.size(); i++) {
      num_ngons += (nverts[i] == 4) ? 0 : 1;
      num_corners += nverts[i];
    }
    mesh->reserve_subd_faces(nverts.size(), num_ngons, num_corners);

    /* create subd_faces */
    int index_offset = 0;

    for (size_t i = 0; i < nverts.size(); i++) {
      mesh->add_subd_face(&verts[index_offset], nverts[i], shader, smooth);
      index_offset += nverts[i];
    }

    /* uv map */
    if (xml_read_float_array(UV, node, "UV")) {
      ustring name = ustring("UVMap");
      Attribute *attr = mesh->subd_attributes.add(ATTR_STD_UV, name);
      float3 *fdata = attr->data_float3();

#if 0
      if (subdivide_uvs) {
        attr->flags |= ATTR_SUBDIVIDED;
      }
#endif

      index_offset = 0;
      for (size_t i = 0; i < nverts.size(); i++) {
        for (int j = 0; j < nverts[i]; j++) {
          *(fdata++) = make_float3(UV[index_offset++]);
        }
      }
    }

    /* setup subd params */
    float dicing_rate = state.dicing_rate;
    xml_read_float(&dicing_rate, node, "dicing_rate");
    dicing_rate = std::max(0.1f, dicing_rate);

    mesh->set_subd_dicing_rate(dicing_rate);
    mesh->set_subd_objecttoworld(state.tfm);
  }

  /* we don't yet support arbitrary attributes, for now add vertex
   * coordinates as generated coordinates if requested */
  if (mesh->need_attribute(state.scene, ATTR_STD_GENERATED)) {
    Attribute *attr = mesh->attributes.add(ATTR_STD_GENERATED);
    memcpy(
        attr->data_float3(), mesh->get_verts().data(), sizeof(float3) * mesh->get_verts().size());
  }
}

/* Light */

static void xml_read_light(XMLReadState &state, xml_node node)
{
  Light *light = new Light();

  light->set_shader(state.shader);
  xml_read_node(state, light, node);

  state.scene->lights.push_back(light);
}

/* Transform */

static void xml_read_transform(xml_node node, Transform &tfm)
{
  if (node.attribute("matrix")) {
    vector<float> matrix;
    if (xml_read_float_array(matrix, node, "matrix") && matrix.size() == 16) {
      ProjectionTransform projection = *(ProjectionTransform *)&matrix[0];
      tfm = tfm * projection_to_transform(projection_transpose(projection));
    }
  }

  if (node.attribute("translate")) {
    float3 translate = zero_float3();
    xml_read_float3(&translate, node, "translate");
    tfm = tfm * transform_translate(translate);
  }

  if (node.attribute("rotate")) {
    float4 rotate = zero_float4();
    xml_read_float4(&rotate, node, "rotate");
    tfm = tfm * transform_rotate(DEG2RADF(rotate.x), make_float3(rotate.y, rotate.z, rotate.w));
  }

  if (node.attribute("scale")) {
    float3 scale = zero_float3();
    xml_read_float3(&scale, node, "scale");
    tfm = tfm * transform_scale(scale);
  }
}

/* State */

static void xml_read_state(XMLReadState &state, xml_node node)
{
  /* read shader */
  string shadername;

  if (xml_read_string(&shadername, node, "shader")) {
    bool found = false;

    foreach (Shader *shader, state.scene->shaders) {
      if (shader->name == shadername) {
        state.shader = shader;
        found = true;
        break;
      }
    }

    if (!found)
      fprintf(stderr, "Unknown shader \"%s\".\n", shadername.c_str());
  }

  xml_read_float(&state.dicing_rate, node, "dicing_rate");

  /* read smooth/flat */
  if (xml_equal_string(node, "interpolation", "smooth"))
    state.smooth = true;
  else if (xml_equal_string(node, "interpolation", "flat"))
    state.smooth = false;
}

/* Scene */

static void xml_read_include(XMLReadState &state, const string &src);

static void xml_read_scene(XMLReadState &state, xml_node scene_node)
{
  for (xml_node node = scene_node.first_child(); node; node = node.next_sibling()) {
    if (string_iequals(node.name(), "film")) {
      xml_read_node(state, state.scene->film, node);
    }
    else if (string_iequals(node.name(), "integrator")) {
      xml_read_node(state, state.scene->integrator, node);
    }
    else if (string_iequals(node.name(), "camera")) {
      xml_read_camera(state, node);
    }
    else if (string_iequals(node.name(), "shader")) {
      xml_read_shader(state, node);
    }
    else if (string_iequals(node.name(), "background")) {
      xml_read_background(state, node);
    }
    else if (string_iequals(node.name(), "mesh")) {
      xml_read_mesh(state, node);
    }
    else if (string_iequals(node.name(), "light")) {
      xml_read_light(state, node);
    }
    else if (string_iequals(node.name(), "transform")) {
      XMLReadState substate = state;

      xml_read_transform(node, substate.tfm);
      xml_read_scene(substate, node);
    }
    else if (string_iequals(node.name(), "state")) {
      XMLReadState substate = state;

      xml_read_state(substate, node);
      xml_read_scene(substate, node);
    }
    else if (string_iequals(node.name(), "include")) {
      string src;

      if (xml_read_string(&src, node, "src"))
        xml_read_include(state, src);
    }
#ifdef WITH_ALEMBIC
    else if (string_iequals(node.name(), "alembic")) {
      xml_read_alembic(state, node);
    }
#endif
    else
      fprintf(stderr, "Unknown node \"%s\".\n", node.name());
  }
}

/* Include */

static void xml_read_include(XMLReadState &state, const string &src)
{
  /* open XML document */
  xml_document doc;
  xml_parse_result parse_result;

  string path = path_join(state.base, src);
  parse_result = doc.load_file(path.c_str());

  if (parse_result) {
    XMLReadState substate = state;
    substate.base = path_dirname(path);

    xml_node cycles = doc.child("cycles");
    xml_read_scene(substate, cycles);
  }
  else {
    fprintf(stderr, "%s read error: %s\n", src.c_str(), parse_result.description());
    exit(EXIT_FAILURE);
  }
}

/* File */

void xml_read_file(Scene *scene, const char *filepath)
{
  XMLReadState state;

  state.scene = scene;
  state.tfm = transform_identity();
  state.shader = scene->default_surface;
  state.smooth = false;
  state.dicing_rate = 1.0f;
  state.base = path_dirname(filepath);

  xml_read_include(state, path_filename(filepath));

  scene->params.bvh_type = BVH_TYPE_STATIC;
}

CCL_NAMESPACE_END
