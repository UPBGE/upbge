/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2024 UPBGE Contributors
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_TypeScriptCompiler.cpp
 *  \ingroup ketsji
 */

#ifdef WITH_TYPESCRIPT

#include "KX_TypeScriptCompiler.h"
#include "CM_Message.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifdef WIN32
#  include <windows.h>
#  include <io.h>
#  define popen _popen
#  define pclose _pclose
#else
#  include <unistd.h>
#endif

bool KX_TypeScriptCompiler::Compile(const std::string &typescript_source,
                                    const std::string &source_name,
                                    std::string &javascript_output)
{
  if (!IsAvailable()) {
    CM_Error("TypeScript compiler (tsc) is not available");
    return false;
  }

  return CompileWithTSC(typescript_source, source_name, javascript_output);
}

bool KX_TypeScriptCompiler::IsAvailable()
{
  // Try to run tsc --version
  FILE *pipe = popen("tsc --version", "r");
  if (!pipe) {
    return false;
  }

  char buffer[128];
  std::string result = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    result += buffer;
  }
  pclose(pipe);

  // If we got output, tsc is available
  return !result.empty();
}

static const char *BGE_DTS_CONTENT =
    "/* BGE runtime globals - injected by UPBGE TypeScript compiler */\n"
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
    "  rayCast(to: [number, number, number] | BGEGameObject, from?: [number, number, number] | BGEGameObject, dist?: number, prop?: string, face?: number, xray?: number, mask?: number): { object: BGEGameObject | null; point: [number, number, number] | null; normal: [number, number, number] | null };\n"
    "  rayCastTo(other: [number, number, number] | BGEGameObject, dist?: number, prop?: string): { object: BGEGameObject | null; point: [number, number, number] | null; normal: [number, number, number] | null };\n"
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
    "  addWheel(wheelObj: BGEGameObject, connectionPoint: [number, number, number], downDir: [number, number, number], axleDir: [number, number, number], suspensionRestLength: number, wheelRadius: number, hasSteering: boolean): void;\n"
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

bool KX_TypeScriptCompiler::CompileWithTSC(const std::string &typescript_source,
                                           const std::string &source_name,
                                           std::string &javascript_output)
{
  // Temporary .ts path: source_name + ".ts" (e.g. "teste.ts" -> "teste.ts.ts")
  std::string temp_ts_file = source_name + ".ts";

  // bge_upbge.d.ts in same directory as .ts so /// <reference path="bge_upbge.d.ts" /> resolves
  std::string::size_type sep = temp_ts_file.find_last_of("/\\");
  std::string dts_path = (sep != std::string::npos) ? temp_ts_file.substr(0, sep + 1) : "";
  dts_path += "bge_upbge.d.ts";

  // Write BGE type declarations so tsc knows about global `bge` and `console`
  std::ofstream dts_file(dts_path);
  if (dts_file.is_open()) {
    dts_file << BGE_DTS_CONTENT;
    dts_file.close();
  }

  // Prepend reference to bge_upbge.d.ts so tsc loads it (avoids "Cannot find name 'bge'")
  std::string ref = "/// <reference path=\"bge_upbge.d.ts\" />\n";
  std::ofstream ts_file(temp_ts_file);
  if (!ts_file.is_open()) {
    CM_Error("Failed to create temporary TypeScript file: " << temp_ts_file);
    remove(dts_path.c_str());
    return false;
  }
  ts_file << ref << typescript_source;
  ts_file.close();

  // Compile with tsc
  std::string command = "tsc --target ES2020 --module none " + temp_ts_file;
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) {
    CM_Error("Failed to execute TypeScript compiler");
    remove(temp_ts_file.c_str());
    remove(dts_path.c_str());
    return false;
  }

  char buffer[128];
  std::string error_output = "";
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    error_output += buffer;
  }
  int status = pclose(pipe);

  // Check if compilation succeeded
  if (status != 0) {
    CM_Error("TypeScript compilation failed: " << error_output);
    remove(temp_ts_file.c_str());
    remove(dts_path.c_str());
    return false;
  }

  // tsc emits .js with same base name as .ts: "x.ts.ts" -> "x.ts.js", "x.ts" -> "x.js"
  std::string js_file = temp_ts_file;
  if (js_file.size() >= 3 && js_file.compare(js_file.size() - 3, 3, ".ts") == 0) {
    js_file.resize(js_file.size() - 3);
    js_file += ".js";
  }
  else {
    js_file += ".js";
  }
  std::ifstream js_stream(js_file);
  if (!js_stream.is_open()) {
    CM_Error("Failed to read compiled JavaScript file: " << js_file);
    remove(temp_ts_file.c_str());
    remove(dts_path.c_str());
    return false;
  }

  std::stringstream js_buffer;
  js_buffer << js_stream.rdbuf();
  javascript_output = js_buffer.str();
  js_stream.close();

  // Clean up temporary files
  remove(temp_ts_file.c_str());
  remove(js_file.c_str());
  remove(dts_path.c_str());

  return true;
}

#endif  // WITH_TYPESCRIPT
