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

  if (!ts_lsp_ensure_started()) {
    return false;
  }

  ts_lsp_ensure_document(content, uri);

  const int completion_id = 2;
  nlohmann::json comp_req = {{"jsonrpc", "2.0"},
                             {"id", completion_id},
                             {"method", "textDocument/completion"},
                             {"params",
                              {{"textDocument", {{"uri", uri}}},
                               {"position", {{"line", lsp_line}, {"character", lsp_char}}}}}};

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

    for (const nlohmann::json &it : items) {
      std::string label = it.value("insertText", it.value("label", ""));
      if (label.empty()) {
        continue;
      }
      char type = tft->format_identifier(label.c_str());
      texttool_suggest_add(label.c_str(), type);
      added++;
    }
    break;
  }

  if (added > 0) {
    texttool_suggest_prefix(seek, seek_len);
  }
  return added > 0;
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
