/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sptext
 *
 * TypeScript/JavaScript LSP (typescript-language-server) over stdio for
 * autocomplete. Uses typescript-language-server --stdio; BGE globals are
 * prepended to the document so the server can suggest e.g. bge.logic.
 */

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "json.hpp"

#include "BLI_process_pipe.h"
#include "BLI_string.h"

#include "MEM_guardedalloc.h"

#include "DNA_text_types.h"

#include "BKE_text.h"
#include "BKE_text_suggestions.h"

#include "text_format.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name BGE d.ts (duplicated from KX_TypeScriptCompiler for editor use)
 * \{ */

static const char *BGE_DTS_CONTENT =
    "/* BGE runtime globals - for LSP autocomplete */\n"
    "interface BGEGameObject {\n"
    "  name: string;\n"
    "  position: [number, number, number];\n"
    "  rotation: [number, number, number];\n"
    "  scale: [number, number, number];\n"
    "  has_physics: boolean;\n"
    "  setPosition(x: number, y: number, z: number): void;\n"
    "  setRotation(euler: [number, number, number] | number, y?: number, z?: number): void;\n"
    "  setScale(scale: [number, number, number] | number, y?: number, z?: number): void;\n"
    "  applyForce(force: [number, number, number], local?: boolean): void;\n"
    "  getVelocity(point?: [number, number, number]): [number, number, number];\n"
    "  getLinearVelocity(local?: boolean): [number, number, number];\n"
    "  setLinearVelocity(vel: [number, number, number], local?: boolean): void;\n"
    "  getAngularVelocity(local?: boolean): [number, number, number];\n"
    "  setAngularVelocity(vel: [number, number, number], local?: boolean): void;\n"
    "  rayCast(to: [number, number, number] | BGEGameObject, from?: [number, number, number] | "
    "BGEGameObject, dist?: number, prop?: string, face?: number, xray?: number, mask?: number): { "
    "object: BGEGameObject | null; point: [number, number, number] | null; normal: [number, "
    "number, number] | null };\n"
    "  rayCastTo(other: [number, number, number] | BGEGameObject, dist?: number, prop?: string): { "
    "object: BGEGameObject | null; point: [number, number, number] | null; normal: [number, "
    "number, number] | null };\n"
    "}\n"
    "interface BGEScene {\n"
    "  objects: BGEGameObject[];\n"
    "  get(name: string): BGEGameObject | null;\n"
    "  activeCamera: BGEGameObject | null;\n"
    "  gravity: [number, number, number];\n"
    "}\n"
    "interface BGESensor { positive: boolean; events: [number, number][]; }\n"
    "interface BGEActuator { name: string; }\n"
    "interface BGEController {\n"
    "  owner: BGEGameObject;\n"
    "  sensors: Record<string, BGESensor>;\n"
    "  actuators: Record<string, BGEActuator>;\n"
    "  activate(act: BGEActuator | string): void;\n"
    "  deactivate(act: BGEActuator | string): void;\n"
    "}\n"
    "type Controller = BGEController;\n"
    "type GameObject = BGEGameObject;\n"
    "type Scene = BGEScene;\n"
    "type Sensor = BGESensor;\n"
    "type Actuator = BGEActuator;\n"
    "interface BGEVehicle {\n"
    "  addWheel(wheelObj: BGEGameObject, connectionPoint: [number, number, number], downDir: "
    "[number, number, number], axleDir: [number, number, number], suspensionRestLength: number, "
    "wheelRadius: number, hasSteering: boolean): void;\n"
    "  getNumWheels(): number;\n"
    "  getWheelPosition(wheelIndex: number): [number, number, number];\n"
    "  getWheelRotation(wheelIndex: number): number;\n"
    "  getWheelOrientationQuaternion(wheelIndex: number): [number, number, number, number];\n"
    "  setSteeringValue(steering: number, wheelIndex: number): void;\n"
    "  applyEngineForce(force: number, wheelIndex: number): void;\n"
    "  applyBraking(braking: number, wheelIndex: number): void;\n"
    "  setTyreFriction(friction: number, wheelIndex: number): void;\n"
    "  setSuspensionStiffness(v: number, i: number): void;\n"
    "  setSuspensionDamping(v: number, i: number): void;\n"
    "  setSuspensionCompression(v: number, i: number): void;\n"
    "  setRollInfluence(v: number, i: number): void;\n"
    "  readonly constraintId: number;\n"
    "  readonly constraintType: number;\n"
    "  rayMask: number;\n"
    "}\n"
    "interface BGECharacter {\n"
    "  jump(): void;\n"
    "  setVelocity(vel: [number, number, number], time?: number, local?: boolean): void;\n"
    "  reset(): void;\n"
    "  readonly onGround: boolean;\n"
    "  gravity: [number, number, number];\n"
    "  fallSpeed: number;\n"
    "  maxJumps: number;\n"
    "  readonly jumpCount: number;\n"
    "  jumpSpeed: number;\n"
    "  maxSlope: number;\n"
    "  walkDirection: [number, number, number];\n"
    "}\n"
    "declare const bge: {\n"
    "  logic: {\n"
    "    getCurrentController(): BGEController | null;\n"
    "    getCurrentScene(): BGEScene | null;\n"
    "    getCurrentControllerObject(): BGEGameObject | null;\n"
    "  };\n"
    "  events: {\n"
    "    WKEY: number; SKEY: number; AKEY: number; DKEY: number;\n"
    "    ACTIVE: number; JUSTACTIVATED?: number; JUSTRELEASED?: number;\n"
    "  };\n"
    "  constraints: {\n"
    "    setGravity(x: number, y: number, z: number): void;\n"
    "    getVehicleConstraint(constraintId: number): BGEVehicle | null;\n"
    "    createVehicle(chassis: BGEGameObject): BGEVehicle | null;\n"
    "    getCharacter(obj: BGEGameObject): BGECharacter | null;\n"
    "  };\n"
    "};\n";

/** \} */

/* -------------------------------------------------------------------- */
/** \name LSP session state
 * \{ */

static BLI_process_pipe *ts_lsp_pipe = nullptr;
static bool ts_lsp_inited = false;
static std::string ts_lsp_uri;
static int ts_lsp_version = 0;

static const int LSP_READ_TIMEOUT_MS = 10000;
static const size_t LSP_READ_BUF_SIZE = 512 * 1024;

/** \} */

/* -------------------------------------------------------------------- */
/** \name JSON-RPC helpers
 * \{ */

static bool lsp_send(BLI_process_pipe *pipe, const nlohmann::json &body)
{
  std::string s = body.dump();
  char header[64];
  const int n = SNPRINTF(header, "Content-Length: %zu\r\n\r\n", s.size());
  if (n < 0 || size_t(n) >= sizeof(header)) {
    return false;
  }
  if (!BLI_process_pipe_write(pipe, header, size_t(n))) {
    return false;
  }
  return BLI_process_pipe_write(pipe, s.c_str(), s.size());
}

/**
 * Read one JSON-RPC message (Content-Length: N + \\r\\n\\r\\n + body).
 * Returns the body as string, or empty on error.
 */
static std::string lsp_read_message(BLI_process_pipe *pipe)
{
  char *buf = static_cast<char *>(MEM_mallocN(LSP_READ_BUF_SIZE, "lsp_read"));
  if (!buf) {
    return "";
  }
  size_t len = 0;
  const char *sep = nullptr;

  while (len < LSP_READ_BUF_SIZE - 1) {
    int n = BLI_process_pipe_read(pipe, buf + len, LSP_READ_BUF_SIZE - 1 - len, LSP_READ_TIMEOUT_MS);
    if (n < 0) {
      MEM_freeN(buf);
      return "";
    }
    if (n == 0) {
      /* Timeout; allow retry a few times? For now fail. */
      MEM_freeN(buf);
      return "";
    }
    len += size_t(n);
    buf[len] = '\0';

    sep = strstr(buf, "\r\n\r\n");
    if (sep) {
      break;
    }
  }

  if (!sep) {
    MEM_freeN(buf);
    return "";
  }

  size_t body_start = (sep - buf) + 4;
  long content_len = -1;
  const char *cl = strstr(buf, "Content-Length:");
  if (cl && cl < sep) {
    content_len = atol(cl + 14);
  }
  if (content_len < 0 || size_t(content_len) > LSP_READ_BUF_SIZE) {
    MEM_freeN(buf);
    return "";
  }

  while (len - body_start < size_t(content_len)) {
    int n = BLI_process_pipe_read(
        pipe, buf + len, LSP_READ_BUF_SIZE - 1 - len, LSP_READ_TIMEOUT_MS);
    if (n <= 0) {
      MEM_freeN(buf);
      return "";
    }
    len += size_t(n);
    buf[len] = '\0';
  }

  std::string out(buf + body_start, size_t(content_len));
  MEM_freeN(buf);
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name LSP lifecycle
 * \{ */

static bool ts_lsp_ensure_started()
{
  if (ts_lsp_pipe && ts_lsp_inited) {
    return BLI_process_pipe_is_alive(ts_lsp_pipe);
  }

  if (ts_lsp_pipe) {
    BLI_process_pipe_destroy(ts_lsp_pipe);
    ts_lsp_pipe = nullptr;
    ts_lsp_inited = false;
  }

  const char *argv[] = {"npx", "typescript-language-server", "--stdio", nullptr};
  ts_lsp_pipe = BLI_process_pipe_create(argv);
  if (!ts_lsp_pipe) {
    return false;
  }

  nlohmann::json init_req = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "initialize"},
      {"params",
       {{"processId", nullptr},
        {"rootUri", nullptr},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", {{"name", "blender"}, {"version", "1.0"}}}}}};

  if (!lsp_send(ts_lsp_pipe, init_req)) {
    BLI_process_pipe_destroy(ts_lsp_pipe);
    ts_lsp_pipe = nullptr;
    return false;
  }

  std::string init_resp = lsp_read_message(ts_lsp_pipe);
  if (init_resp.empty()) {
    BLI_process_pipe_destroy(ts_lsp_pipe);
    ts_lsp_pipe = nullptr;
    return false;
  }

  nlohmann::json initialized = {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nlohmann::json::object()}};
  if (!lsp_send(ts_lsp_pipe, initialized)) {
    BLI_process_pipe_destroy(ts_lsp_pipe);
    ts_lsp_pipe = nullptr;
    return false;
  }

  ts_lsp_inited = true;
  ts_lsp_uri.clear();
  ts_lsp_version = 0;
  return true;
}

static void ts_lsp_ensure_document(const std::string &content, const std::string &uri)
{
  if (uri != ts_lsp_uri) {
    if (!ts_lsp_uri.empty()) {
      nlohmann::json did_close = {
          {"jsonrpc", "2.0"}, {"method", "textDocument/didClose"}, {"params", {{"textDocument", {{"uri", ts_lsp_uri}}}}}};
      lsp_send(ts_lsp_pipe, did_close);
    }
    ts_lsp_uri = uri;
    ts_lsp_version = 1;

    std::string full = std::string(BGE_DTS_CONTENT) + "\n" + content;
    const char *lang = (uri.find(".ts") != std::string::npos) ? "typescript" : "javascript";

    nlohmann::json did_open = {{"jsonrpc", "2.0"},
                               {"method", "textDocument/didOpen"},
                               {"params",
                                {{"textDocument",
                                  {{"uri", uri},
                                   {"languageId", lang},
                                   {"version", ts_lsp_version},
                                   {"text", full}}}}}};
    lsp_send(ts_lsp_pipe, did_open);
  }
  else {
    ts_lsp_version += 1;
    std::string full = std::string(BGE_DTS_CONTENT) + "\n" + content;
    int end_line = 1;
    for (char c : full) {
      if (c == '\n') {
        end_line++;
      }
    }
    nlohmann::json did_change = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params",
         {{"textDocument", {{"uri", uri}, {"version", ts_lsp_version}}},
          {"contentChanges",
           {{{"range",
              {{"start", {{"line", 0}, {"character", 0}}},
                {"end", {{"line", end_line}, {"character", 0}}}}},
             {"text", full}}}}}}};
    lsp_send(ts_lsp_pipe, did_change);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Type inference helper
 * \{ */

/**
 * Get the type information at a specific position using textDocument/hover.
 * Returns the type name if available, or empty string.
 */
static std::string ts_lsp_get_type_at_position(BLI_process_pipe *pipe,
                                                const std::string &uri,
                                                int line,
                                                int character)
{
  const int hover_id = 4;
  nlohmann::json hover_req = {{"jsonrpc", "2.0"},
                              {"id", hover_id},
                              {"method", "textDocument/hover"},
                              {"params",
                               {{"textDocument", {{"uri", uri}}},
                                {"position", {{"line", line}, {"character", character}}}}}};

  if (!lsp_send(pipe, hover_req)) {
    return "";
  }

  /* Read response with timeout */
  std::string body = lsp_read_message(pipe);
  if (body.empty()) {
    return "";
  }

  nlohmann::json j;
  try {
    j = nlohmann::json::parse(body);
  }
  catch (...) {
    return "";
  }

  if (!j.contains("id") || j["id"] != hover_id) {
    return "";
  }

  nlohmann::json result = j.value("result", nlohmann::json::object());
  if (result.is_null() || !result.contains("contents")) {
    return "";
  }

  /* Extract type information from hover result */
  nlohmann::json contents = result["contents"];
  std::string type_info = "";

  if (contents.is_string()) {
    type_info = contents.get<std::string>();
  }
  else if (contents.is_array() && !contents.empty()) {
    nlohmann::json first = contents[0];
    if (first.is_string()) {
      type_info = first.get<std::string>();
    }
    else if (first.is_object() && first.contains("value")) {
      type_info = first["value"].get<std::string>();
    }
  }
  else if (contents.is_object() && contents.contains("value")) {
    type_info = contents["value"].get<std::string>();
  }

  /* Extract type name from type info string.
   * Examples:
   * - "(property) Controller.owner: GameObject" -> "Controller"
   * - "(method) Controller.activate(act: BGEActuator | string): void" -> "Controller"
   * - "Controller" -> "Controller"
   * - "BGEController" -> "BGEController"
   */
  
  /* Look for type name patterns */
  /* First, try to find type before a dot (property/method access) */
  size_t dot_pos = type_info.find('.');
  if (dot_pos != std::string::npos) {
    /* Find the start of the type name before the dot */
    size_t start = 0;
    /* Look backwards from dot to find type name start */
    for (int i = int(dot_pos) - 1; i >= 0; i--) {
      char c = type_info[i];
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
        start = size_t(i);
      }
      else if (c == ' ' || c == ')') {
        start = size_t(i + 1);
        break;
      }
      else {
        break;
      }
    }
    if (start < dot_pos) {
      std::string type_name = type_info.substr(start, dot_pos - start);
      if (!type_name.empty()) {
        return type_name;
      }
    }
  }
  
  /* If no dot found, try to extract type name from the string */
  /* Look for common patterns like "Controller" or "BGEController" */
  if (type_info.find("Controller") != std::string::npos) {
    /* Check if it's BGE or local */
    if (type_info.find("BGE") != std::string::npos && type_info.find("BGEController") != std::string::npos) {
      return "BGEController";
    }
    else if (type_info.find("Controller") != std::string::npos) {
      /* Try to extract just "Controller" */
      size_t pos = type_info.find("Controller");
      if (pos > 0 && type_info[pos - 1] == 'B' && type_info[pos - 2] == 'G' && type_info[pos - 3] == 'E') {
        return "BGEController";
      }
      return "Controller";
    }
  }
  
  /* Similar for other common types */
  if (type_info.find("GameObject") != std::string::npos) {
    if (type_info.find("BGEGameObject") != std::string::npos) {
      return "BGEGameObject";
    }
    return "GameObject";
  }

  return type_info;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

bool text_format_is_js_or_ts(const TextFormatType *tft)
{
  if (!tft || !tft->ext) {
    return false;
  }
  for (const char **ext = tft->ext; *ext; ext++) {
    if (BLI_strcasecmp(*ext, "js") == 0 || BLI_strcasecmp(*ext, "mjs") == 0 ||
        BLI_strcasecmp(*ext, "cjs") == 0 || BLI_strcasecmp(*ext, "ts") == 0 ||
        BLI_strcasecmp(*ext, "mts") == 0 || BLI_strcasecmp(*ext, "cts") == 0)
    {
      return true;
    }
  }
  return false;
}

bool text_format_is_typescript(const TextFormatType *tft)
{
  if (!tft || !tft->ext) {
    return false;
  }
  for (const char **ext = tft->ext; *ext; ext++) {
    if (BLI_strcasecmp(*ext, "ts") == 0 || BLI_strcasecmp(*ext, "mts") == 0 ||
        BLI_strcasecmp(*ext, "cts") == 0)
    {
      return true;
    }
  }
  return false;
}

bool ts_lsp_get_completions(Text *text, const char *seek, int seek_len, TextFormatType *tft)
{
  if (!text || !text->curl || !tft) {
    return false;
  }

  size_t content_len = 0;
  char *raw = txt_to_buf(text, &content_len);
  if (!raw) {
    return false;
  }
  std::string content(raw, content_len);
  MEM_freeN(raw);

  /* 0-based line of the first line of user content in the full doc (BGE_DTS + "\n" + content). */
  int line_offset = 1;
  for (const char *p = BGE_DTS_CONTENT; *p; p++) {
    if (*p == '\n') {
      line_offset++;
    }
  }

  int line_0 = 0;
  for (TextLine *ln = static_cast<TextLine *>(text->lines.first); ln; ln = ln->next) {
    if (ln == text->curl) {
      break;
    }
    line_0++;
  }
  int lsp_line = line_offset + line_0;
  int lsp_char = text->curc; /* Byte offset; LSP expects UTF-16 but many servers accept this. */

  std::string uri = "untitled:";
  uri += (text->id.name + 2);

  /* Detect if this is a TypeScript file */
  bool is_typescript = (uri.find(".ts") != std::string::npos);

  /* Detect context: are we after a dot (property/method access)? */
  bool after_dot = false;
  /* Detect if we're inside a typed object literal (e.g., const x: Type = { ... }) */
  bool in_typed_object_literal = false;
  
  if (text->curl && text->curc > 0) {
    const char *line = text->curl->line;
    int pos = text->curc - 1;
    /* Skip whitespace backwards */
    while (pos > 0 && (line[pos] == ' ' || line[pos] == '\t')) {
      pos--;
    }
    if (pos >= 0 && line[pos] == '.') {
      after_dot = true;
    }
    
    /* Check if we're inside an object literal after a type annotation */
    /* Look for pattern: "const/let/var name: Type = {" */
    if (is_typescript && !after_dot) {
      int check_pos = text->curc - 1;
      int brace_depth = 0;
      bool found_opening_brace = false;
      
      /* Look backwards to find the opening brace of the object literal */
      while (check_pos >= 0) {
        if (line[check_pos] == '}') {
          brace_depth++;
        }
        else if (line[check_pos] == '{') {
          if (brace_depth == 0) {
            found_opening_brace = true;
            break;
          }
          brace_depth--;
        }
        check_pos--;
      }
      
      if (found_opening_brace) {
        /* Look backwards from the opening brace for "= {" pattern */
        int before_brace = check_pos - 1;
        while (before_brace >= 0 && (line[before_brace] == ' ' || line[before_brace] == '\t')) {
          before_brace--;
        }
        if (before_brace >= 0 && line[before_brace] == '=') {
          /* Look for type annotation before = */
          int before_eq = before_brace - 1;
          while (before_eq >= 0 && (line[before_eq] == ' ' || line[before_eq] == '\t')) {
            before_eq--;
          }
          if (before_eq >= 0 && line[before_eq] == ':') {
            /* Found pattern: ": Type = {" - we're in a typed object literal */
            in_typed_object_literal = true;
          }
        }
      }
    }
  }

  if (!ts_lsp_ensure_started()) {
    return false;
  }

  ts_lsp_ensure_document(content, uri);

  /* The TypeScript language server needs time to process document changes before it can
   * provide intelligent completions based on type inference (e.g., after "as Controller").
   * We send the completion request immediately; the server should handle it, but if it
   * hasn't finished processing changes, it may return less accurate results.
   * 
   * Note: For most TypeScript/JavaScript code, byte offsets work fine for character positions
   * since most characters are ASCII. The LSP spec requires UTF-16 code units, but many
   * servers accept byte offsets for ASCII text. */

  /* For TypeScript, get the type information to ensure we only show
   * properties/methods from the inferred type */
  std::string inferred_type = "";
  
  if (is_typescript && text->curc > 0) {
    const char *line = text->curl->line;
    
    if (after_dot) {
      /* Get type at position before the dot */
      int type_check_char = text->curc - 1;
      /* Skip whitespace backwards to find the actual identifier */
      while (type_check_char > 0 && (line[type_check_char] == ' ' || line[type_check_char] == '\t' || line[type_check_char] == '.')) {
        type_check_char--;
      }
      /* Find the start of the identifier */
      while (type_check_char > 0 && 
             ((line[type_check_char - 1] >= 'a' && line[type_check_char - 1] <= 'z') ||
              (line[type_check_char - 1] >= 'A' && line[type_check_char - 1] <= 'Z') ||
              (line[type_check_char - 1] >= '0' && line[type_check_char - 1] <= '9') ||
              line[type_check_char - 1] == '_')) {
        type_check_char--;
      }
      int type_check_line = lsp_line;
      inferred_type = ts_lsp_get_type_at_position(ts_lsp_pipe, uri, type_check_line, type_check_char);
    }
    else if (in_typed_object_literal) {
      /* Get the type of the variable being assigned (the expected type of the object literal) */
      /* Look backwards for the variable name and its type annotation */
      int check_pos = text->curc - 1;
      int brace_depth = 0;
      
      /* Find the opening brace */
      while (check_pos >= 0) {
        if (line[check_pos] == '}') {
          brace_depth++;
        }
        else if (line[check_pos] == '{') {
          if (brace_depth == 0) {
            break;
          }
          brace_depth--;
        }
        check_pos--;
      }
      
      if (check_pos >= 0 && line[check_pos] == '{') {
        /* Look backwards for "= {" */
        int before_brace = check_pos - 1;
        while (before_brace >= 0 && (line[before_brace] == ' ' || line[before_brace] == '\t')) {
          before_brace--;
        }
        if (before_brace >= 0 && line[before_brace] == '=') {
          /* Look backwards for type annotation ": Type" */
          int before_eq = before_brace - 1;
          while (before_eq >= 0 && (line[before_eq] == ' ' || line[before_eq] == '\t')) {
            before_eq--;
          }
          if (before_eq >= 0 && line[before_eq] == ':') {
            /* Get type name after ':' */
            int type_start = before_eq + 1;
            while (type_start < before_brace && (line[type_start] == ' ' || line[type_start] == '\t')) {
              type_start++;
            }
            int type_end = type_start;
            while (type_end < before_brace && 
                   ((line[type_end] >= 'a' && line[type_end] <= 'z') ||
                    (line[type_end] >= 'A' && line[type_end] <= 'Z') ||
                    (line[type_end] >= '0' && line[type_end] <= '9') ||
                    line[type_end] == '_')) {
              type_end++;
            }
            if (type_end > type_start) {
              inferred_type = std::string(line + type_start, type_end - type_start);
            }
          }
        }
      }
    }
  }

  const int completion_id = 2;
  nlohmann::json comp_req = {{"jsonrpc", "2.0"},
                             {"id", completion_id},
                             {"method", "textDocument/completion"},
                             {"params",
                              {{"textDocument", {{"uri", uri}}},
                               {"position", {{"line", lsp_line}, {"character", lsp_char}}},
                               /* Include context to help with type inference.
                                * triggerKind: 1 = Invoked (manual trigger via CTRL+SPACE) */
                               {"context", {{"triggerKind", 1}}}}}};

  if (!lsp_send(ts_lsp_pipe, comp_req)) {
    return false;
  }

  int added = 0;
  while (true) {
    std::string body = lsp_read_message(ts_lsp_pipe);
    if (body.empty()) {
      break;
    }
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(body);
    }
    catch (...) {
      continue;
    }
    if (!j.contains("id") || j["id"] != completion_id) {
      continue; /* Notification or other; discard and read next. */
    }

    nlohmann::json result = j.value("result", nlohmann::json::object());
    nlohmann::json items = result.is_array() ? result : result.value("items", nlohmann::json::array());
    if (!items.is_array()) {
      break;
    }

    /* Keywords to filter out */
    static const char *keywords[] = {
        "const", "let", "var", "function", "class", "interface", "type", "enum",
        "if", "else", "for", "while", "do", "switch", "case", "default",
        "return", "break", "continue", "throw", "try", "catch", "finally",
        "import", "export", "from", "as", "namespace", "module", "declare",
        "async", "await", "yield", "new", "this", "super", "extends", "implements",
        "static", "public", "private", "protected", "readonly", "abstract",
        "true", "false", "null", "undefined", "void", "any", "unknown", "never",
        "string", "number", "boolean", "bigint", "symbol", "object",
        nullptr
    };

    for (const nlohmann::json &it : items) {
      std::string label = it.value("insertText", it.value("label", ""));
      if (label.empty()) {
        continue;
      }

      /* For TypeScript, apply strict filtering */
      if (is_typescript) {
        int kind = it.value("kind", 0);

        /* If we're inside a typed object literal, apply VERY strict filtering */
        if (in_typed_object_literal) {
          /* First, filter by kind - only Property, Field, or Variable */
          if (kind != 5 && kind != 9 && kind != 6) {
            continue;
          }
          
          /* Filter out keywords and type names */
          if (label == "const" || label == "let" || label == "var" ||
              label == "interface" || label == "type" || label == "class" ||
              label == "string" || label == "number" || label == "boolean" ||
              label == "void" || label == "any" || label == "unknown" ||
              label == "true" || label == "false" || label == "null" || label == "undefined") {
            continue;
          }
          
          /* CRITICAL: For typed object literals, we MUST have detail that mentions the type */
          std::string detail = it.value("detail", "");
          
          if (!inferred_type.empty()) {
            /* We know the expected type - require that detail mentions it */
            if (detail.empty()) {
              /* No detail = not a valid property suggestion */
              continue;
            }
            
            std::string detail_lower = detail;
            std::transform(detail_lower.begin(), detail_lower.end(), detail_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
            std::string inferred_lower = inferred_type;
            std::transform(inferred_lower.begin(), inferred_lower.end(), inferred_lower.begin(),
                         [](unsigned char c) { return std::tolower(c); });
            
            /* Detail MUST mention the type name (e.g., "Teste.ola" or "(property) Teste.ola: string") */
            if (detail_lower.find(inferred_lower) == std::string::npos) {
              /* Detail doesn't mention our type - filter it out */
              continue;
            }
          }
          else {
            /* Even without inferred type, require detail that looks like a property */
            if (detail.empty() || 
                (detail.find("property") == std::string::npos && 
                 detail.find("field") == std::string::npos &&
                 detail.find(":") == std::string::npos)) {
              /* Doesn't look like a property - filter it out */
              continue;
            }
          }
        }
        /* If we're after a dot, only show properties and methods */
        else if (after_dot) {
          /* LSP CompletionItemKind:
           * 2 = Method
           * 5 = Property
           * 6 = Variable (sometimes used for properties)
           * 9 = Field (property-like)
           */
          if (kind != 2 && kind != 5 && kind != 6 && kind != 9) {
            continue;
          }

          /* If we have inferred type information, filter based on it to ensure
           * we only show properties/methods from the inferred type */
          if (!inferred_type.empty()) {
            std::string detail = it.value("detail", "");
            /* Check if this suggestion belongs to the inferred type */
            bool belongs_to_inferred = false;
            
            if (!detail.empty()) {
              /* Check if detail mentions the inferred type */
              std::string detail_lower = detail;
              std::transform(detail_lower.begin(), detail_lower.end(), detail_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
              std::string inferred_lower = inferred_type;
              std::transform(inferred_lower.begin(), inferred_lower.end(), inferred_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
              
              belongs_to_inferred = (detail_lower.find(inferred_lower) != std::string::npos);
              
              /* If inferred type is a local interface (not BGE) and detail only mentions BGE types,
               * this suggestion doesn't belong to the inferred type */
              if (inferred_type.find("BGE") == std::string::npos && 
                  detail_lower.find("bge") != std::string::npos && 
                  !belongs_to_inferred) {
                continue;
              }
            }
            
            /* Additional check: if we have a local type and the suggestion's detail
             * doesn't mention it at all, but mentions BGE types, filter it out */
            if (inferred_type.find("BGE") == std::string::npos && !detail.empty()) {
              std::string detail_lower = detail;
              std::transform(detail_lower.begin(), detail_lower.end(), detail_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
              std::string inferred_lower = inferred_type;
              std::transform(inferred_lower.begin(), inferred_lower.end(), inferred_lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
              
              bool mentions_bge = (detail_lower.find("bge") != std::string::npos);
              bool mentions_local = (detail_lower.find(inferred_lower) != std::string::npos);
              
              /* If detail mentions BGE types but not the local type, filter it out */
              if (mentions_bge && !mentions_local) {
                continue;
              }
            }
          }
        }
        else {
          /* When not after a dot, filter out:
           * - Text (1)
           * - Snippet (15)
           * - Keywords (14) - but we'll filter manually for better control
           */
          if (kind == 1 || kind == 15) {
            continue;
          }
        }

        /* Filter out keywords */
        bool is_keyword = false;
        for (const char **kw = keywords; *kw; kw++) {
          if (label == *kw) {
            is_keyword = true;
            break;
          }
        }
        if (is_keyword) {
          continue;
        }

        /* Filter out single non-alphanumeric characters (except _) */
        if (label.length() == 1 && !((label[0] >= 'a' && label[0] <= 'z') ||
                                    (label[0] >= 'A' && label[0] <= 'Z') ||
                                    (label[0] >= '0' && label[0] <= '9') ||
                                    label[0] == '_')) {
          continue;
        }
        
        /* For TypeScript, apply AGGRESSIVE filtering: only show suggestions that are
         * clearly type-based (have detail with type information) */
        if (!after_dot && !in_typed_object_literal) {
          /* When not in a type-specific context, require detail that indicates type information */
          std::string detail = it.value("detail", "");
          if (detail.empty()) {
            /* No detail = generic suggestion, filter it out */
            continue;
          }
          
          /* Detail should contain type information (:, property, field, interface, etc.) */
          std::string detail_lower = detail;
          std::transform(detail_lower.begin(), detail_lower.end(), detail_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
          
          /* Require that detail looks like type information */
          bool has_type_info = (detail_lower.find(":") != std::string::npos ||
                               detail_lower.find("property") != std::string::npos ||
                               detail_lower.find("field") != std::string::npos ||
                               detail_lower.find("method") != std::string::npos ||
                               detail_lower.find("interface") != std::string::npos ||
                               detail_lower.find("type") != std::string::npos ||
                               detail_lower.find("class") != std::string::npos);
          
          if (!has_type_info) {
            /* Doesn't look like type-based information - filter it out */
            continue;
          }
        }
      }
      
      /* FINAL FILTER: For typed object literals, remove ANY suggestion that doesn't
       * clearly belong to the expected type. This is the last line of defense against
       * generic file-based suggestions. */
      if (in_typed_object_literal) {
        std::string detail = it.value("detail", "");
        
        if (!inferred_type.empty()) {
          /* We know the expected type - detail MUST mention it */
          if (detail.empty()) {
            continue; /* No detail = not a valid property */
          }
          
          std::string detail_lower = detail;
          std::transform(detail_lower.begin(), detail_lower.end(), detail_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
          std::string inferred_lower = inferred_type;
          std::transform(inferred_lower.begin(), inferred_lower.end(), inferred_lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
          
          /* Detail MUST contain the type name */
          if (detail_lower.find(inferred_lower) == std::string::npos) {
            continue; /* Doesn't mention our type = filter it out */
          }
        }
        else {
          /* Even without inferred type, require detail that looks like a property */
          if (detail.empty() || 
              (detail.find("property") == std::string::npos && 
               detail.find("field") == std::string::npos &&
               detail.find(":") == std::string::npos)) {
            continue; /* Doesn't look like a property */
          }
        }
      }

      char type = tft->format_identifier(label.c_str());
      texttool_suggest_add(label.c_str(), type);
      added++;
    }
    break;
  }

  if (added > 0) {
    texttool_suggest_prefix(seek, seek_len);
    return true;
  }
  
  /* For TypeScript, ALWAYS return true to prevent fallback to word search.
   * This ensures we ONLY show type-based suggestions from LSP, never file-based words.
   * If LSP doesn't return valid suggestions, we show nothing rather than generic words. */
  if (is_typescript) {
    return true;
  }
  
  return false;
}

void ts_lsp_shutdown(void)
{
  if (!ts_lsp_pipe) {
    return;
  }

  const int shutdown_id = 3;
  nlohmann::json shutdown_req = {
      {"jsonrpc", "2.0"}, {"id", shutdown_id}, {"method", "shutdown"}, {"params", nullptr}};
  lsp_send(ts_lsp_pipe, shutdown_req);

  std::string body = lsp_read_message(ts_lsp_pipe);
  (void)body;

  nlohmann::json exit_notif = {{"jsonrpc", "2.0"}, {"method", "exit"}};
  lsp_send(ts_lsp_pipe, exit_notif);

  BLI_process_pipe_destroy(ts_lsp_pipe);
  ts_lsp_pipe = nullptr;
  ts_lsp_inited = false;
  ts_lsp_uri.clear();
  ts_lsp_version = 0;
}

/** \} */

}  // namespace blender
