/* SPDX-FileCopyrightText: 2026 UPBGE Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * The differential tester (lean runtime plan, phase A3): random node trees compiled by the real
 * compiler and run in the manager's order (LN_DiffTrees.h), compared with themselves and with
 * goldens kept from the production setup.
 *
 * - Variants that must end alike: serial recording against tree batches (helpers {0,1,3,5} x
 *   chunks {1,2,7,64}). Same text, byte for byte (every command field, three flush orders, the
 *   world, every tree's state).
 * - Goldens (lean_golden/differential_v0.txt): a hash of the production run's text for each seed.
 *   A change of the runtime that changes what any tree records, applies or keeps shows as the first
 *   seed whose hash moved. Record again with LN_RECORD_GOLDEN=1 after a change that was meant.
 * - The soak (UPBGE_LN_DIFF_COUNT seeds from UPBGE_LN_DIFF_SEED, report to UPBGE_LN_DIFF_REPORT)
 *   runs more seeds through the same checks and lists the differences of the pairs that are known
 *   to differ (register evaluator against the tree evaluator ...), with a smaller tree for each.
 *
 * A failing seed prints itself, its tree and a smaller tree that fails the same way. */

#include "testing/testing.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include "LN_DiffTrees.h"

using namespace ln_test;

namespace {

using diff::GeneratedTree;
using diff::TreeGen;

uint64_t EnvNumber(const char *name, const uint64_t fallback)
{
  const char *text = std::getenv(name);
  return text != nullptr ? std::strtoull(text, nullptr, 10) : fallback;
}

/** Compiles `tree`; "" when it compiled, else the first line of the report. */
std::string CompileFailure(GeneratedTree &tree)
{
  const std::shared_ptr<LN_Program> program = LN_TreeCompiler().Compile(tree.tree);
  if (program == nullptr) {
    return "no program";
  }
  if (!program->GetCompileReport().HasErrors()) {
    return "";
  }
  const std::string report = FormatCompileReport(program->GetCompileReport());
  return report.substr(0, report.find('\n'));
}

struct Compiled {
  std::unique_ptr<GeneratedTree> tree;
  std::shared_ptr<LN_Program> program;
};

/** The tree of `seed` compiled; no program if it did not compile. */
Compiled CompileSeed(const uint64_t seed,
                     const int budget = 10,
                     const int depth = 3,
                     const std::set<std::string> &exclude = {},
                     const bool finders = false,
                     const bool accepted = false)
{
  Compiled compiled;
  TreeGen generator{seed, budget, depth};
  generator.Exclude(exclude);
  generator.SetObjectFinders(finders);
  generator.SetAcceptedFamilies(accepted);
  compiled.tree = generator.Build();
  compiled.program = LN_TreeCompiler().Compile(compiled.tree->tree);
  if (compiled.program != nullptr && compiled.program->GetCompileReport().HasErrors()) {
    compiled.program = nullptr;
  }
  return compiled;
}

/** The world of a seed: 1, 3 or 17 objects, properties from the seed. */
diff::RunOptions OptionsFor(const uint64_t seed)
{
  static const uint32_t sizes[] = {1, 3, 17};
  diff::Rng pick(seed ^ 0x51ED270Bull);
  diff::RunOptions options;
  options.objects = sizes[pick.below(3)];
  options.world_seed = seed * 7919 + 13;
  return options;
}

/** One of the 16 (helpers, chunks) pairs, by number. */
void SetBatch(diff::RunOptions &options, const uint64_t number)
{
  static const uint32_t helpers[] = {0, 1, 3, 5};
  static const uint32_t chunks[] = {1, 2, 7, 64};
  options.helpers = helpers[number % 4];
  options.chunks = chunks[(number / 4) % 4];
}

/** A variant of the reference run: a name and the options that differ from the reference. */
struct Variant {
  std::string name;
  diff::RunOptions options;
};

/** The variants that must end exactly as the reference does. `number` picks the tree batch
 * setting (one of the 16 helper x chunk pairs); `all_batches`: all of them. */
std::vector<Variant> MustMatchVariants(const Compiled &compiled,
                                       const diff::RunOptions &base,
                                       const uint64_t number,
                                       const bool all_batches,
                                       const int only = -1)
{
  std::vector<Variant> variants;
  if (only != 0 && compiled.program->GetTreeBatchProfile().eligible) {
    for (uint64_t setting = 0; setting < 16; setting++) {
      if (!all_batches && setting != number % 16) {
        continue;
      }
      Variant batch{"", base};
      SetBatch(batch.options, setting);
      batch.name = "tree batch, " + std::to_string(batch.options.helpers) + " helpers, " +
                   std::to_string(batch.options.chunks) + " chunks";
      variants.push_back(batch);
    }
  }
  return variants;
}

/** What differs between the reference run of a program and its variants, or "" when they all end
 * alike. `reference_hash`: the reference run's hash when it is known (the goldens made it), which
 * saves running it again; a difference is then described by running it. */
std::string CompareVariants(const Compiled &compiled,
                            const diff::RunOptions &base,
                            const uint64_t number,
                            const bool all_batches,
                            const std::string *reference_hash = nullptr,
                            const int only = -1)
{
  std::optional<diff::RunResult> reference;
  const auto get_reference = [&]() -> const diff::RunResult & {
    if (!reference) {
      reference = diff::RunWorld(compiled.program, base);
    }
    return *reference;
  };
  for (const Variant &variant : MustMatchVariants(compiled, base, number, all_batches, only)) {
    const diff::RunResult other = diff::RunWorld(compiled.program, variant.options);
    if (!other.failure.empty()) {
      return variant.name + ": " + other.failure;
    }
    if (reference_hash != nullptr && *reference_hash == GoldenHash(other.text) &&
        other.warnings == 0)
    {
      continue; /* the same hash as the reference run's */
    }
    const diff::RunResult &ref = get_reference();
    if (other.text != ref.text || other.warnings != ref.warnings) {
      return variant.name + ": " + diff::FirstDifference(ref.text, other.text);
    }
  }
  return "";
}

/** The same for the tree of `seed` made with the given sizes. */
std::string VariantDifference(const uint64_t seed,
                              const int budget,
                              const int depth,
                              const bool all_batches)
{
  Compiled compiled = CompileSeed(seed, budget, depth);
  if (compiled.program == nullptr) {
    return "";
  }
  return CompareVariants(compiled, OptionsFor(seed), seed, all_batches);
}

/** The seed's tree, then the smallest tree of the same seed (budget x depth) that still fails
 * `still_fails`, for a report. */
std::string TreeReport(const uint64_t seed,
                       const std::function<bool(int budget, int depth)> &still_fails)
{
  Compiled full = CompileSeed(seed);
  std::string out = "seed " + std::to_string(seed) + " (objects " +
                    std::to_string(OptionsFor(seed).objects) + ")\n" + full.tree->Describe();
  for (int size = 1; size <= 40; size++) {
    for (int depth = 0; depth <= 3; depth++) {
      const int budget = size;
      if (budget == 10 && depth == 3) {
        continue;
      }
      if (still_fails(budget, depth)) {
        Compiled small = CompileSeed(seed, budget, depth);
        out += "smaller tree that fails alike (budget " + std::to_string(budget) + ", depth " +
               std::to_string(depth) + "):\n" + small.tree->Describe();
        return out;
      }
    }
    if (size >= 9) {
      break;
    }
  }
  return out;
}

uint64_t NormalSeedCount()
{
  return 300;
}

/** The production run of each seed: hash and size of its text, and the kinds it ran. */
struct RunSummary {
  bool compiled = false;
  std::string hash;
  uint64_t tree_runs = 0;
  uint64_t commands = 0;
  std::map<std::string, uint32_t> kinds;
  std::string failure;
};

RunSummary Summarize(const Compiled &compiled, const diff::RunOptions &options)
{
  RunSummary summary;
  if (compiled.program == nullptr) {
    return summary;
  }
  summary.compiled = true;
  const diff::RunResult run = diff::RunWorld(compiled.program, options);
  summary.hash = GoldenHash(run.text);
  summary.tree_runs = run.tree_runs;
  summary.kinds = diff::KindsOf(*compiled.program);
  for (size_t at = run.text.find("type="); at != std::string::npos;
       at = run.text.find("type=", at + 1))
  {
    summary.commands++;
  }
  if (!run.failure.empty() || run.warnings != 0) {
    summary.failure = run.failure + (run.warnings != 0 ? "warnings printed\n" : "");
  }
  return summary;
}

/** The production run of a fixed seed; the goldens, the variants and the coverage read the same
 * runs, made once. */
const RunSummary &SeedSummary(const uint64_t seed)
{
  static std::map<uint64_t, RunSummary> cache;
  auto found = cache.find(seed);
  if (found == cache.end()) {
    found = cache.emplace(seed, Summarize(CompileSeed(seed), OptionsFor(seed))).first;
  }
  return found->second;
}

/* ---- every setting of every family, alone ---- */

/** A family setting as a tree of its own, run in a world: the table's settings, each output and
 * each type a generic node takes, so every kind the table can make runs whatever the random
 * seeds chose. */
struct Probe {
  std::string name;
  Compiled compiled;
  diff::RunOptions options;
};

/** Calls `visit(name, tree)` for every probe tree of the table (compiled or not). `fold`: the
 * tree reads its properties as constants of these values (the folded twin of the probe). */
void ForEachProbe(
    const std::function<void(const std::string &, std::unique_ptr<GeneratedTree>)> &visit,
    const diff::FoldValues *fold = nullptr)
{
  for (const diff::Family &family : diff::FamilyTable()) {
    const std::vector<int16_t> first = family.custom1.empty() ? std::vector<int16_t>{0} :
                                                                family.custom1;
    const std::vector<int16_t> second = family.custom2.empty() ? std::vector<int16_t>{0} :
                                                                 family.custom2;
    const std::vector<diff::Cat> generics = family.generic.empty() ?
                                                std::vector<diff::Cat>{diff::Cat::None} :
                                                family.generic;
    for (const int16_t custom1 : first) {
      for (const int16_t custom2 : second) {
        const std::string name = std::string(family.idname + 11) + " c1=" +
                                 std::to_string(custom1) + " c2=" + std::to_string(custom2);
        if (family.role == diff::Role::Data) {
          TreeGen shape{1};
          const size_t outputs = shape.OutputCount(family);
          for (const diff::Cat generic : generics) {
            for (size_t output = 0; output < outputs; output++) {
              TreeGen generator{7};
              generator.SetFold(fold);
              std::unique_ptr<GeneratedTree> tree = generator.ProbeData(
                  family, custom1, custom2, output, generic);
              diff::GenNode &node = *tree->nodes[1];
              if (!node.output_allowed(output) ||
                  diff::CategoryOf(node.out_pins[output].type) == diff::Cat::Exec)
              {
                continue;
              }
              visit(name + " out=" + node.out_pins[output].identifier +
                        " type=" + std::to_string(int(generic)),
                    std::move(tree));
            }
          }
        }
        else {
          TreeGen generator{7};
          generator.SetFold(fold);
          visit(name, generator.ProbeFlow(family, custom1, custom2));
        }
      }
    }
  }
}

/** The probes that compile and have something to run (a flow node behind On Update, a data node
 * written to a property or printed). */
std::vector<Probe> &Probes()
{
  static std::vector<Probe> probes = [] {
    std::vector<Probe> list;
    ForEachProbe([&](const std::string &name, std::unique_ptr<GeneratedTree> tree) {
      Probe probe;
      probe.name = name;
      probe.compiled.tree = std::move(tree);
      if (probe.compiled.tree->nodes.size() < 2) {
        return; /* a lone event */
      }
      probe.compiled.program = LN_TreeCompiler().Compile(probe.compiled.tree->tree);
      if (probe.compiled.program == nullptr || probe.compiled.program->GetCompileReport().HasErrors()) {
        return; /* reported by EveryFamilySettingCompilesAlone */
      }
      probe.options.objects = 3;
      probe.options.frames = 20;
      probe.options.world_seed = 5000 + list.size();
      list.push_back(std::move(probe));
    });
    return list;
  }();
  return probes;
}

const RunSummary &ProbeSummary(const size_t index)
{
  static std::map<size_t, RunSummary> cache;
  auto found = cache.find(index);
  if (found == cache.end()) {
    found = cache.emplace(index, Summarize(Probes()[index].compiled, Probes()[index].options)).first;
  }
  return found->second;
}

/* Every setting of every family of the table, alone, with its inputs constants or property reads:
 * the table lists only what the compiler builds (so a random tree that fails to compile is the
 * generator's links, not a setting). */
TEST(LN_LeanDifferential, EveryFamilySettingCompilesAlone)
{
  size_t checked = 0;
  ForEachProbe([&](const std::string &name, std::unique_ptr<GeneratedTree> tree) {
    checked++;
    const std::string failure = CompileFailure(*tree);
    EXPECT_EQ(failure, "") << name;
  });
  EXPECT_GT(checked, 100u);
}

/* Random trees almost always compile (the plan fails the generator below 70%). */
TEST(LN_LeanDifferential, GeneratedTreesCompile)
{
  const uint64_t total = NormalSeedCount();
  uint64_t compiled = 0;
  std::map<std::string, int> reasons;
  for (uint64_t seed = 1; seed <= total; seed++) {
    TreeGen generator{seed};
    std::unique_ptr<GeneratedTree> tree = generator.Build();
    const std::string failure = CompileFailure(*tree);
    if (failure.empty()) {
      compiled++;
    }
    else {
      reasons[failure]++;
      if (std::getenv("LN_DIFF_VERBOSE") != nullptr) {
        std::printf("seed %llu: %s\n%s\n", (unsigned long long)seed, failure.c_str(),
                    tree->Describe().c_str());
      }
    }
  }
  for (const auto &entry : reasons) {
    std::printf("  %4d x %s\n", entry.second, entry.first.c_str());
  }
  EXPECT_GE(compiled * 100, total * 90) << compiled << " of " << total << " compiled";
}

/* Plan C2 P0 (c2_p0/family_probe.py): how often the generated trees hold each family given by
 * UPBGE_LN_DIFF_EXTRA_FAMILIES and how many of those compile; a family the trees never hold
 * compiled tests nothing. Skipped without the variable. */
TEST(LN_LeanDifferential, ExtraFamiliesAreReached)
{
  if (std::getenv("UPBGE_LN_DIFF_EXTRA_FAMILIES") == nullptr) {
    GTEST_SKIP() << "only with UPBGE_LN_DIFF_EXTRA_FAMILIES";
  }
  std::map<std::string, std::pair<int, int>> held; /* idname: (trees, compiled trees) */
  for (const auto &[idname, weight] : diff::ProbedFamilyNames()) {
    held[idname] = {0, 0};
  }
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    Compiled compiled = CompileSeed(seed);
    std::set<std::string> in_tree;
    for (const auto &node : compiled.tree->nodes) {
      in_tree.insert(node->node.idname);
    }
    for (auto &[idname, counts] : held) {
      if (in_tree.count(idname) != 0) {
        counts.first++;
        counts.second += compiled.program != nullptr ? 1 : 0;
      }
    }
  }
  for (const auto &[idname, counts] : held) {
    std::printf("extra family %s: in %d trees, %d compiled\n", idname.c_str(), counts.first,
                counts.second);
    EXPECT_GE(counts.second, 10) << idname << " is in too few compiled trees";
  }
}

/* Plan C2 P0: trees that also take the nodes of the accepted list (Family::accepted_row, each
 * probed alone first by c2_p0/family_probe.py) end alike in every way the fixed seeds are
 * compared: a run again, the reference mode against the lean VM, the block walk against compiled
 * routes, both memos off, and a tree batch. Seeds of their own (the fixed seeds keep their goldens
 * and their share of batch-safe trees); the columns compare them in ColumnsEndAsTheLeanVM.
 * UPBGE_LN_DIFF_ACCEPTED_SEEDS (default 300) and _FIRST widen or move the sweep. */
TEST(LN_LeanDifferential, AcceptedFamiliesEndAlike)
{
  const uint64_t seeds = EnvNumber("UPBGE_LN_DIFF_ACCEPTED_SEEDS", 300);
  const uint64_t first = EnvNumber("UPBGE_LN_DIFF_ACCEPTED_FIRST", 1);
  size_t compiled_count = 0, batches = 0, failures = 0;
  std::set<std::string> reached;
  for (uint64_t seed = first; seed < first + seeds && failures < 5; seed++) {
    Compiled compiled = CompileSeed(seed + 700000, 10, 3, {}, false, true);
    if (compiled.program == nullptr) {
      continue;
    }
    compiled_count++;
    for (const auto &node : compiled.tree->nodes) {
      reached.insert(node->node.idname);
    }
    diff::RunOptions production = OptionsFor(seed);
    production.lean = 2;
    const diff::RunResult a = diff::RunWorld(compiled.program, production);
    EXPECT_EQ(a.failure, "") << "accepted seed " << seed;
    const auto expect_same = [&](const char *name, const diff::RunOptions &options) {
      const diff::RunResult b = diff::RunWorld(compiled.program, options);
      if (a.text != b.text || a.warnings != b.warnings) {
        failures++;
        ADD_FAILURE() << "accepted seed " << seed << " (" << name
                      << "): " << diff::FirstDifference(a.text, b.text) << "\n"
                      << compiled.tree->Describe();
      }
    };
    expect_same("again", production);
    diff::RunOptions reference = production;
    reference.lean = 0;
    expect_same("reference mode", reference);
    diff::RunOptions walk = production;
    walk.routes = 0;
    expect_same("block walk", walk);
    diff::RunOptions memo_off = production;
    memo_off.memo = 0;
    memo_off.lean_memo = 0;
    expect_same("memos off", memo_off);
    if (compiled.program->GetTreeBatchProfile().eligible) {
      diff::RunOptions batch = production;
      SetBatch(batch, seed % 16);
      expect_same("tree batch", batch);
      batches++;
    }
  }
  size_t rows = 0, rows_reached = 0;
  for (const diff::Family &family : diff::FamilyTable()) {
    if (family.accepted_row) {
      rows++;
      rows_reached += reached.count(family.idname);
    }
  }
  std::printf("accepted families: %zu of %llu trees compiled, %zu in a tree batch; %zu of %zu "
              "accepted nodes in them\n",
              compiled_count,
              (unsigned long long)seeds,
              batches,
              rows_reached,
              rows);
  EXPECT_GT(compiled_count * 10, seeds * 7);
  if (seeds >= 300) {
    EXPECT_GT(rows_reached * 10, rows * 9) << "the accepted nodes are hardly in the trees";
  }
}

/* The same seed gives the same text twice: nothing in a run reads uninitialized memory or an
 * address. */
TEST(LN_LeanDifferential, ARunIsDeterministic)
{
  size_t compared = 0;
  for (uint64_t seed = 1; seed <= 16; seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    const diff::RunResult a = diff::RunWorld(compiled.program, OptionsFor(seed));
    const diff::RunResult b = diff::RunWorld(compiled.program, OptionsFor(seed));
    EXPECT_TRUE(a.text == b.text) << "seed " << seed << ": " << diff::FirstDifference(a.text, b.text);
    EXPECT_FALSE(a.text.empty());
    compared++;
  }
  EXPECT_GT(compared, 12u);
}

/* Typed streams and the one command list, serial recording and tree batches (any number of
 * helpers and chunks) end exactly alike: every recorded field, the three flush orders, the
 * world after the flush and the state of every tree. Every tree a batch may hold and every eighth of
 * the others from the fixed set, with one batch setting a seed (all 16 in the soak), and every
 * setting of the table. */
TEST(LN_LeanDifferential, VariantsOfARunEndAlike)
{
  size_t compared = 0, batches = 0;
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    /* Every tree a batch may hold, and every eighth of the others. */
    const bool eligible = compiled.program->GetTreeBatchProfile().eligible;
    if (!eligible && seed % 8 != 0) {
      continue;
    }
    /* The legacy stream for the even seeds, a batch for the odd ones that may have one. */
    const bool batch = eligible && seed % 2 == 1;
    const std::string difference = CompareVariants(
        compiled, OptionsFor(seed), seed, false, &SeedSummary(seed).hash, batch ? 1 : 0);
    compared++;
    batches += batch ? 1 : 0;
    if (!difference.empty()) {
      ADD_FAILURE() << difference << "\n"
                    << TreeReport(seed, [&](const int budget, const int depth) {
                         return !VariantDifference(seed, budget, depth, false).empty();
                       });
      break;
    }
  }
  EXPECT_GT(compared, 60u);
  EXPECT_GT(batches, 20u) << "too few batch eligible trees: the batch path is hardly tested";

  size_t probe_batches = 0;
  for (size_t index = 0; index < Probes().size(); index++) {
    Probe &probe = Probes()[index];
    /* A probe is one node: the legacy stream for half of them, a batch for the others. */
    const std::string difference = CompareVariants(
        probe.compiled, probe.options, index, false, &ProbeSummary(index).hash, int(index % 2));
    probe_batches += probe.compiled.program->GetTreeBatchProfile().eligible ? 1 : 0;
    EXPECT_EQ(difference, "") << probe.name;
  }
  EXPECT_GT(probe_batches, 10u);
}

/* Goldens v0: the production run (typed streams, register evaluator, serial recording) of each of
 * the fixed seeds and of every setting of the table, as one hash of its whole text. Any change of
 * what a tree records, applies or keeps moves a hash. */
TEST(LN_LeanDifferential, TheProductionRunsMatchTheGoldens)
{
  std::string lines;
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    const RunSummary &summary = SeedSummary(seed);
    EXPECT_TRUE(summary.failure.empty()) << "seed " << seed << ": " << summary.failure;
    lines += "seed " + std::to_string(seed) + " " +
             (summary.compiled ? summary.hash + " runs=" + std::to_string(summary.tree_runs) +
                                     " commands=" + std::to_string(summary.commands) :
                                 std::string("not compiled")) +
             "\n";
  }
  /* The accepted rows (plan C2 P0) whose probe warns: a node acting on Blender data the rig has
   * not (a mesh, a modifier, an action) warns that its command failed, the same in every mode. */
  std::set<std::string> accepted;
  for (const diff::Family &family : diff::FamilyTable()) {
    if (family.accepted_row) {
      accepted.insert(std::string(family.idname + 11) + " ");
    }
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    const RunSummary &summary = ProbeSummary(index);
    const std::string &name = Probes()[index].name;
    const bool may_warn = summary.failure == "warnings printed\n" &&
                          accepted.count(name.substr(0, name.find(' ') + 1)) != 0;
    EXPECT_TRUE(summary.failure.empty() || may_warn) << name << ": " << summary.failure;
    lines += "setting " + Probes()[index].name + " " + summary.hash +
             " runs=" + std::to_string(summary.tree_runs) + "\n";
  }
  const std::string verdict = CheckGolden("differential_v0", lines);
  EXPECT_EQ(verdict, "") << verdict;
}

/* The lean VM (plan phase A16) against the reference mode (the generic handlers; the old
 * interpreter until plan step R4, and the goldens were recorded from it): the whole text of a run
 * (commands, applied world, every tree's state) is the same for each fixed seed and each setting
 * of the table, in the serial recording and, for the batch eligible trees, in a batch on a pool
 * (lean programs are made on the main thread and only read on the helpers). */
TEST(LN_LeanDifferential, TheLeanVMMatchesTheReferenceMode)
{
  size_t compared = 0, batches = 0;
  const uint64_t runs_before = LN_LeanTreeRunCount();
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    diff::RunOptions old_way = OptionsFor(seed);
    old_way.lean = 0;
    diff::RunOptions lean = OptionsFor(seed);
    lean.lean = 2;
    const diff::RunResult a = diff::RunWorld(compiled.program, old_way);
    const diff::RunResult b = diff::RunWorld(compiled.program, lean);
    compared++;
    EXPECT_EQ(a.text, b.text) << "seed " << seed << ": " << diff::FirstDifference(a.text, b.text);
    if (compiled.program->GetTreeBatchProfile().eligible && seed % 2 == 1) {
      diff::RunOptions batch = lean;
      batch.chunks = 2;
      batch.helpers = 3;
      const diff::RunResult c = diff::RunWorld(compiled.program, batch);
      batches++;
      EXPECT_EQ(a.text, c.text) << "seed " << seed << " (batch): "
                                << diff::FirstDifference(a.text, c.text);
    }
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    Probe &probe = Probes()[index];
    diff::RunOptions old_way = probe.options;
    old_way.lean = 0;
    diff::RunOptions lean = probe.options;
    lean.lean = 2;
    const diff::RunResult a = diff::RunWorld(probe.compiled.program, old_way);
    const diff::RunResult b = diff::RunWorld(probe.compiled.program, lean);
    EXPECT_EQ(a.text, b.text) << probe.name << ": " << diff::FirstDifference(a.text, b.text);
  }
  EXPECT_GT(compared, 60u);
  EXPECT_GT(batches, 10u);
  EXPECT_GT(LN_LeanTreeRunCount(), runs_before) << "no tree ran on the lean VM";
}

/* Compiled routes (plan phase A33) against the lean VM's block walk: the whole text of a run
 * (commands, applied world, every tree's state, bookkeeping included in the state meaning) is the
 * same with routes on and off, for each fixed seed and probe, in Auto and Force, serial and in a
 * tree batch. The routes must really run (LN_RouteRunCount). */
TEST(LN_LeanDifferential, CompiledRoutesMatchTheBlockWalk)
{
  size_t compared = 0, batches = 0, routed = 0;
  const uint64_t routes_before = LN_RouteRunCount();
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    compiled.program->EnsureLeanProgram();
    const std::shared_ptr<const LN_LeanProgram> &lean_program = compiled.program->GetLeanProgram();
    if (lean_program != nullptr &&
        std::any_of(lean_program->events.begin(),
                    lean_program->events.end(),
                    [](const LN_LeanEvent &event) { return event.has_route; }))
    {
      routed++;
    }
    for (const int lean : {1, 2}) {
      diff::RunOptions walk = OptionsFor(seed);
      walk.lean = lean;
      walk.routes = 0;
      diff::RunOptions route = walk;
      route.routes = 1;
      const diff::RunResult a = diff::RunWorld(compiled.program, walk);
      const diff::RunResult b = diff::RunWorld(compiled.program, route);
      compared++;
      EXPECT_EQ(a.text, b.text) << "seed " << seed << " lean " << lean << ": "
                                << diff::FirstDifference(a.text, b.text);
      EXPECT_EQ(a.warnings, b.warnings) << "seed " << seed;
      if (lean == 2 && compiled.program->GetTreeBatchProfile().eligible && seed % 2 == 0) {
        diff::RunOptions batch = route;
        batch.chunks = 2;
        batch.helpers = 3;
        const diff::RunResult c = diff::RunWorld(compiled.program, batch);
        batches++;
        EXPECT_EQ(a.text, c.text) << "seed " << seed << " (batch): "
                                  << diff::FirstDifference(a.text, c.text);
      }
    }
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    Probe &probe = Probes()[index];
    diff::RunOptions walk = probe.options;
    walk.lean = 2;
    walk.routes = 0;
    diff::RunOptions route = walk;
    route.routes = 1;
    const diff::RunResult a = diff::RunWorld(probe.compiled.program, walk);
    const diff::RunResult b = diff::RunWorld(probe.compiled.program, route);
    EXPECT_EQ(a.text, b.text) << probe.name << ": " << diff::FirstDifference(a.text, b.text);
  }
  std::printf("compiled routes: %zu of %zu programs have one\n", routed, compared / 2);
  EXPECT_GT(compared, 120u);
  EXPECT_GT(batches, 5u);
  EXPECT_GT(routed, 10u);
  EXPECT_GT(LN_RouteRunCount(), routes_before) << "no event ran as a compiled route";
}

/* The lean memo (plan phase A33) changes no result: for each fixed seed and probe, the reference
 * mode, the lean VM with routes and its memo on, and the same with the memo off give the same text
 * (also with the per-tick memo switch off). */
TEST(LN_LeanDifferential, TheLeanMemoChangesNoResult)
{
  size_t compared = 0, memoized = 0;
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    compiled.program->EnsureLeanProgram();
    const std::shared_ptr<const LN_LeanProgram> &lean_program = compiled.program->GetLeanProgram();
    if (lean_program != nullptr && lean_program->memo_count > 0) {
      memoized++;
    }
    diff::RunOptions old_way = OptionsFor(seed);
    old_way.lean = 0;
    diff::RunOptions memo_on = OptionsFor(seed);
    memo_on.lean = 2;
    memo_on.routes = 1;
    memo_on.lean_memo = 1;
    diff::RunOptions memo_off = memo_on;
    memo_off.lean_memo = 0;
    diff::RunOptions no_memo = memo_on;
    no_memo.memo = 0;
    const diff::RunResult a = diff::RunWorld(compiled.program, old_way);
    const diff::RunResult b = diff::RunWorld(compiled.program, memo_on);
    const diff::RunResult c = diff::RunWorld(compiled.program, memo_off);
    const diff::RunResult d = diff::RunWorld(compiled.program, no_memo);
    compared++;
    EXPECT_EQ(a.text, b.text) << "seed " << seed << ": " << diff::FirstDifference(a.text, b.text);
    EXPECT_EQ(b.text, c.text) << "seed " << seed << " (memo off): "
                              << diff::FirstDifference(b.text, c.text);
    EXPECT_EQ(b.text, d.text) << "seed " << seed << " (no memo at all): "
                              << diff::FirstDifference(b.text, d.text);
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    Probe &probe = Probes()[index];
    diff::RunOptions old_way = probe.options;
    old_way.lean = 0;
    diff::RunOptions memo_on = probe.options;
    memo_on.lean = 2;
    memo_on.routes = 1;
    memo_on.lean_memo = 1;
    const diff::RunResult a = diff::RunWorld(probe.compiled.program, old_way);
    const diff::RunResult b = diff::RunWorld(probe.compiled.program, memo_on);
    EXPECT_EQ(a.text, b.text) << probe.name << ": " << diff::FirstDifference(a.text, b.text);
  }
  std::printf("lean memo: %zu of %zu programs keep an expression\n", memoized, compared);
  EXPECT_GT(compared, 60u);
  EXPECT_GT(memoized, 5u);
}

/* Plan C1: a generated tree whose program lowers to columns ends every tick with the same world in
 * columns as on the lean VM (world only: a column run records no commands and keeps no tree
 * bookkeeping), also with batches of the others; the reasons the other programs stay on the VM are
 * printed (the next nodes worth a kernel). UPBGE_LN_DIFF_COLUMN_SEEDS widens the sweep. */
TEST(LN_LeanDifferential, ColumnsEndAsTheLeanVM)
{
  const uint64_t seeds = EnvNumber("UPBGE_LN_DIFF_COLUMN_SEEDS", 2000);
  size_t compiled_count = 0, lowered = 0, compared = 0;
  std::map<std::string, size_t> refusals;
  const uint64_t lanes_before = LN_ColumnLaneCount() + LN_ColumnDirectLaneCount();
  /* Half the seeds from every family, half from the families columns have kernels for (the rest
   * left out of the generator), so most of those lower. */
  static const std::set<std::string> column_families = {
      "LogicNativeOnUpdate",
      "LogicNativeOnFixedUpdate",
      "LogicNativeBranch",
      "LogicNativeSetGamePropertyFloat",
      "LogicNativeSetGamePropertyInt",
      "LogicNativeSetGamePropertyBool",
      "LogicNativeApplyMovement",
      "LogicNativeApplyRotation",
      "LogicNativeOnce",
      "LogicNativeFlowRouter",
      "LogicNativeRepeatLimit",
      "LogicNativeSetTreeProperty",
      "LogicNativeMath",
      "LogicNativeCompare",
      "LogicNativeVectorMath",
      "LogicNativeCombineXYZ",
      "LogicNativeSeparateXYZ",
      "LogicNativeClampValue",
      "LogicNativeThreshold",
      "LogicNativeRangedThreshold",
      "LogicNativeRandomFloat",
      "LogicNativeRandomInt",
      "LogicNativeTypecast",
      "LogicNativeGetTreeProperty",
      "LogicNativeGetAxisVector",
      "LogicNativeGetObjectAttribute",
      "LogicNativeGetDistance",
      "LogicNativeGate",
      "LogicNativeValueSwitch",
      "LogicNativeBooleanEdge",
      "LogicNativeFlowGate",
      "LogicNativeTranslate",
      "LogicNativeMoveToward",
      "LogicNativeRotateToward",
      "LogicNativeAlignAxisToVector",
      "LogicNativeHasProperty",
      "LogicNativeGetGravity",
      "LogicNativeStoreValue",
      "LogicNativeRandomVector",
      "LogicNativeVectorRotate",
      "LogicNativeDelay",
      "LogicNativeTimer",
      "LogicNativePulsify",
      "LogicNativeCooldown",
      "LogicNativeOnNextFrame",
      "LogicNativeBarrier",
      "LogicNativeSetObjectAttribute",
      "LogicNativeVectorToRotation",
      "LogicNativeApplyRotation",
      "LogicNativeEuler",
      "LogicNativeColorRGBA",
  };
  /* A family probed by UPBGE_LN_DIFF_EXTRA_FAMILIES (plan C2 P0) joins the column families. */
  std::set<std::string> probed;
  for (const auto &[idname, weight] : diff::ProbedFamilyNames()) {
    probed.insert(idname);
  }
  std::set<std::string> others;
  for (const diff::Family &family : diff::FamilyTable()) {
    if (column_families.count(family.idname) == 0 && probed.count(family.idname) == 0) {
      others.insert(family.idname);
    }
  }
  /* UPBGE_LN_DIFF_COLUMN_FIRST: the first seed (to run one seed of a soak again). */
  const char *first_text = std::getenv("UPBGE_LN_DIFF_COLUMN_FIRST");
  const uint64_t first = first_text != nullptr ? std::strtoull(first_text, nullptr, 10) : 1;
  size_t finder_lowered = 0, finder_lanes_runs = 0, p2_runs = 0, p2_lanes_runs = 0;
  size_t direct_programs = 0;
  for (uint64_t seed = first; seed < first + seeds; seed++) {
    /* A third from every family, a third from the column families, a third from the column
     * families with objects found while playing and averages (plan C1-9) in a finder world. */
    const bool finders = seed % 3 == 2;
    const bool restricted = seed % 3 != 1;
    /* (The third from every family takes the accepted rows too: plan C2 P0.) */
    Compiled compiled = finders    ? CompileSeed(seed + 900000, 10, 3, others, true) :
                        restricted ? CompileSeed(seed + 500000, 10, 3, others) :
                                     CompileSeed(seed, 10, 3, {}, false, true);
    if (compiled.program == nullptr) {
      continue;
    }
    compiled_count++;
    bool lowers = false;
    for (const LN_Event event : {LN_Event::OnUpdate, LN_Event::OnFixedUpdate}) {
      std::string refusal;
      if (LN_CompileColumnProgram(*compiled.program, event, &refusal) != nullptr) {
        lowers = true;
      }
      else if (!refusal.empty()) {
        /* The reason without the node's own words (the names of a property or object). */
        refusals[refusal.substr(0, refusal.find('\''))]++;
      }
    }
    if (lowers) {
      lowered++;
      finder_lowered += finders ? 1 : 0;
    }
    /* Variants: serial, tree batches, and (plan C2 P2) owners in a hierarchy, with the tree
     * twice on every owner every other seed. A program the compiler refuses runs as a direct
     * program (plan C2 P5, Auto: without a compiled route; serial and in a hierarchy). */
    const uint64_t direct_before = LN_ColumnDirectLaneCount();
    for (const int variant : {0, 1, 2}) {
      if (!lowers && variant == 1) {
        continue;
      }
      diff::RunOptions vm = OptionsFor(seed);
      vm.states = false;
      vm.world_only = true;
      vm.lean = 2;
      vm.finder_world = finders;
      if (variant == 1) {
        vm.helpers = 3;
        vm.chunks = 2;
      }
      if (variant == 2) {
        vm.parented_owners = true;
        vm.two_trees = seed % 2 == 0;
        p2_runs += lowers ? 1 : 0;
      }
      diff::RunOptions columns = vm;
      columns.columns = true;
      const diff::RunResult a = diff::RunWorld(compiled.program, vm);
      const diff::RunResult b = diff::RunWorld(compiled.program, columns);
      compared++;
      /* UPBGE_LN_DIFF_DUMP_DIR: both texts of a run that differs, as files (vm_/columns_<seed>_
       * <variant>.txt), for a diff of the whole run. */
      if (a.text != b.text && std::getenv("UPBGE_LN_DIFF_DUMP_DIR") != nullptr) {
        const std::string dir = std::getenv("UPBGE_LN_DIFF_DUMP_DIR");
        const std::string stem = std::to_string(seed) + "_" + std::to_string(variant) + ".txt";
        std::ofstream(dir + "/vm_" + stem) << a.text;
        std::ofstream(dir + "/columns_" + stem)
            << b.text << "\n---- program\n"
            << LN_DumpProgramText(*compiled.program) << "\n---- tree\n"
            << compiled.tree->Describe();
      }
      finder_lanes_runs += lowers && finders && b.column_lanes != 0 ? 1 : 0;
      p2_lanes_runs += lowers && variant == 2 && b.column_lanes != 0 ? 1 : 0;
      EXPECT_EQ(a.text, b.text) << "seed " << seed
                                << (finders ? " (finders)" : restricted ? " (column families)" : "")
                                << " variant " << variant
                                << (variant == 2 ? (seed % 2 == 0 ? " (parented, two trees)" :
                                                                    " (parented)") :
                                                   "")
                                << ": "
                                << diff::FirstDifference(a.text, b.text) << "\n"
                                << compiled.tree->Describe()
                                << (std::getenv("LN_DIFF_VERBOSE") != nullptr && a.text != b.text ?
                                        LN_DumpProgramText(*compiled.program) :
                                        std::string());
      EXPECT_EQ(a.warnings, b.warnings) << "seed " << seed;
      EXPECT_EQ(b.failure, "") << "seed " << seed;
      if (b.column_lanes == 0 && lowers && std::getenv("LN_DIFF_VERBOSE") != nullptr) {
        std::printf("seed %llu ran no lanes: %s", (unsigned long long)seed, b.column_refusals.c_str());
      }
    }
    if (!lowers && LN_ColumnDirectLaneCount() != direct_before) {
      direct_programs++;
    }
  }
  std::printf("columns: %zu refused programs ran as direct programs\n", direct_programs);
  EXPECT_GT(direct_programs, 0u) << "no refused program ran as a direct program";
  std::printf("columns: %zu of %zu generated programs lower (%zu runs compared); with object "
              "finders %zu lower, %zu runs formed lanes\n",
              lowered,
              compiled_count,
              compared,
              finder_lowered,
              finder_lanes_runs);
  /* (Every program a direct program: no lanes, the passes compared all the same.) */
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_GT(finder_lanes_runs, 0u) << "no tree with object finders ran in columns";
  }
  std::printf("columns: %zu runs with parented owners, %zu formed lanes\n", p2_runs, p2_lanes_runs);
  if (LN_GetColumnDirectMode() != LN_ColumnDirectMode::Force) {
    EXPECT_GT(p2_lanes_runs, 0u) << "no tree of a parented owner ran in columns";
  }
  std::vector<std::pair<size_t, std::string>> sorted;
  for (const auto &entry : refusals) {
    sorted.emplace_back(entry.second, entry.first);
  }
  std::sort(sorted.rbegin(), sorted.rend());
  for (size_t index = 0; index < sorted.size() && index < 25; index++) {
    std::printf("  %5zu  %s\n", sorted[index].first, sorted[index].second.c_str());
  }
  EXPECT_GT(LN_ColumnLaneCount() + LN_ColumnDirectLaneCount(), lanes_before)
      << "no tree ran in columns";
}

/* Every kind the trees reach ran at least 50 times (as many times as it appears in a program times
 * the tree runs), and the number of kinds not reached is printed: the generator's blind spots,
 * for the runtime phases that come after. */
TEST(LN_LeanDifferential, EveryKindTheTreesReachRanOften)
{
  /* `ran_by_table`: the runs of the fixed seeds and of the table's own rows' probes. */
  std::map<std::string, uint64_t> ran, ran_by_table;
  const auto add = [&](const RunSummary &summary, const bool by_table) {
    for (const auto &kind : summary.kinds) {
      ran[kind.first] += uint64_t(kind.second) * summary.tree_runs;
      if (by_table) {
        ran_by_table[kind.first] += uint64_t(kind.second) * summary.tree_runs;
      }
    }
  };
  std::set<std::string> accepted;
  for (const diff::Family &family : diff::FamilyTable()) {
    if (family.accepted_row) {
      accepted.insert(std::string(family.idname + 11) + " ");
    }
  }
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    add(SeedSummary(seed), true);
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    const std::string &name = Probes()[index].name;
    add(ProbeSummary(index), accepted.count(name.substr(0, name.find(' ') + 1)) == 0);
  }
  /* The accepted rows' kinds run in AcceptedFamiliesEndAlike's trees too (plan C2 P0). */
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    add(Summarize(CompileSeed(seed + 700000, 10, 3, {}, false, true), OptionsFor(seed)), false);
  }
  size_t count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&count);
  size_t unreached = 0;
  for (size_t index = 0; index < count; index++) {
    if (ran.find(rows[index].name) == ran.end()) {
      unreached++;
    }
  }
  for (const auto &entry : ran) {
    if (ran_by_table.count(entry.first) != 0) {
      EXPECT_GE(entry.second, 50u) << entry.first << " ran only " << entry.second << " times";
    }
    else {
      /* Made only by accepted rows: at least a whole probe tree's runs (3 objects x 8 frames). */
      EXPECT_GE(entry.second, 24u) << entry.first << " ran only " << entry.second << " times";
    }
  }
  std::printf("generated trees reach %zu of %zu kinds (%zu not reached; the table's own rows "
              "%zu)\n",
              ran.size(),
              count,
              unreached,
              ran_by_table.size());
  EXPECT_GT(ran.size(), 80u);
}

/* The soak: more seeds through the same checks, then the pairs that are known to differ. Only
 * with UPBGE_LN_DIFF_COUNT; the report goes to stdout and to UPBGE_LN_DIFF_REPORT. */
TEST(LN_LeanDifferential, Soak)
{
  const uint64_t count = EnvNumber("UPBGE_LN_DIFF_COUNT", 0);
  if (count == 0) {
    GTEST_SKIP() << "set UPBGE_LN_DIFF_COUNT (and UPBGE_LN_DIFF_SEED) to soak";
  }
  const uint64_t first = EnvNumber("UPBGE_LN_DIFF_SEED", 1);
  std::string report = "seeds " + std::to_string(first) + " .. " + std::to_string(first + count - 1) +
                       "\n";
  uint64_t compiled_count = 0, must_match_failures = 0, register_compared = 0,
           register_different = 0, lean_compared = 0, lean_different = 0;
  std::vector<std::string> lean_examples;
  std::map<std::string, uint64_t> ran;
  std::vector<std::string> register_examples;
  for (uint64_t seed = first; seed < first + count; seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    compiled_count++;
    const diff::RunResult reference = diff::RunWorld(compiled.program, OptionsFor(seed));
    for (const auto &kind : diff::KindsOf(*compiled.program)) {
      ran[kind.first] += uint64_t(kind.second) * reference.tree_runs;
    }
    if (!reference.failure.empty() || reference.warnings != 0) {
      must_match_failures++;
      report += "seed " + std::to_string(seed) + ": the reference run itself failed: " +
                reference.failure + (reference.warnings != 0 ? " (warnings)\n" : "\n");
    }
    const std::string difference = VariantDifference(seed, 10, 3, true);
    if (!difference.empty()) {
      must_match_failures++;
      report += "MUST MATCH, differs: " + difference + "\n" +
                TreeReport(seed, [&](const int budget, const int depth) {
                  return !VariantDifference(seed, budget, depth, true).empty();
                });
    }
    /* Must match since plan phase A16: the lean VM equals the reference mode (the old interpreter
     * until plan step R4). */
    {
      diff::RunOptions old_way = OptionsFor(seed);
      old_way.lean = 0;
      diff::RunOptions lean = OptionsFor(seed);
      lean.lean = 2;
      const diff::RunResult a = diff::RunWorld(compiled.program, old_way);
      const diff::RunResult b = diff::RunWorld(compiled.program, lean);
      lean_compared++;
      if (a.text != b.text) {
        lean_different++;
        must_match_failures++;
        if (lean_examples.size() < 6) {
          lean_examples.push_back("seed " + std::to_string(seed) + ": " +
                                  diff::FirstDifference(a.text, b.text) +
                                  (std::getenv("LN_DIFF_VERBOSE") != nullptr ?
                                       "\n" + compiled.tree->Describe() +
                                           LN_DumpProgramText(*compiled.program) :
                                       std::string()));
        }
      }
    }
    /* Must match since plan phase A15: the per-tick memo never changes a result. */
    diff::RunOptions memo_off = OptionsFor(seed);
    memo_off.memo = 0;
    diff::RunOptions memo_on = OptionsFor(seed);
    memo_on.memo = 1;
    const diff::RunResult a = diff::RunWorld(compiled.program, memo_on);
    const diff::RunResult b = diff::RunWorld(compiled.program, memo_off);
    register_compared++;
    if (a.text != b.text) {
      register_different++;
      must_match_failures++;
      if (register_examples.size() < 6) {
        register_examples.push_back("seed " + std::to_string(seed) + ": " +
                                    diff::FirstDifference(a.text, b.text));
      }
    }
  }
  /* Known to differ (or not): profiling on, and constants the compiler folds against the same
   * values read at runtime (the probes, twelve worlds). */
  uint64_t profiled_compared = 0, profiled_different = 0;
  std::vector<std::string> profiled_examples;
  for (uint64_t seed = first; seed < first + std::min<uint64_t>(count, 400); seed++) {
    Compiled compiled = CompileSeed(seed);
    if (compiled.program == nullptr) {
      continue;
    }
    diff::RunOptions profiled = OptionsFor(seed);
    profiled.profiled = true;
    const diff::RunResult a = diff::RunWorld(compiled.program, OptionsFor(seed));
    const diff::RunResult b = diff::RunWorld(compiled.program, profiled);
    profiled_compared++;
    if (a.text != b.text) {
      profiled_different++;
      must_match_failures++;
      if (profiled_examples.size() < 6) {
        profiled_examples.push_back("seed " + std::to_string(seed) + ": " +
                                    diff::FirstDifference(a.text, b.text));
      }
    }
  }
  uint64_t fold_compared = 0, fold_different = 0;
  std::map<std::string, std::vector<std::string>> fold_examples;
  std::set<std::string> accepted;
  for (const diff::Family &family : diff::FamilyTable()) {
    if (family.accepted_row) {
      accepted.insert(std::string(family.idname + 11) + " ");
    }
  }
  for (uint64_t number = 0; number < 12; number++) {
    const uint64_t world_seed = 9000 + number;
    const diff::FoldValues values = diff::FoldValuesFor(world_seed);
    std::vector<std::pair<std::string, Compiled>> live, folded;
    const auto collect = [&](const diff::FoldValues *fold,
                             std::vector<std::pair<std::string, Compiled>> &into) {
      ForEachProbe(
          [&](const std::string &name, std::unique_ptr<GeneratedTree> tree) {
            Compiled item;
            item.tree = std::move(tree);
            if (item.tree->nodes.size() >= 2) {
              item.program = LN_TreeCompiler().Compile(item.tree->tree);
              if (item.program != nullptr && item.program->GetCompileReport().HasErrors()) {
                item.program = nullptr;
              }
            }
            into.emplace_back(name, std::move(item));
          },
          fold);
    };
    collect(nullptr, live);
    collect(&values, folded);
    diff::RunOptions options;
    options.objects = 2;
    options.frames = 6;
    options.states = false;
    options.identical_objects = true;
    options.world_seed = world_seed;
    for (size_t index = 0; index < live.size(); index++) {
      const std::string &name = live[index].first;
      if (live[index].second.program == nullptr || folded[index].second.program == nullptr ||
          name.find("Random") != std::string::npos || name.find("TreeProperty") != std::string::npos)
      {
        continue; /* a Random value follows the expression index (D2); tree properties are state */
      }
      const diff::RunResult a = diff::RunWorld(live[index].second.program, options);
      const diff::RunResult b = diff::RunWorld(folded[index].second.program, options);
      fold_compared++;
      if (a.final_world != b.final_world || a.warnings != b.warnings) {
        fold_different++;
        /* An accepted row's (plan C2 P0) differences are reported, not must-match: the folded
         * twin assumes a tree never writes what it reads (Modify Property may) and folds a
         * generic pin by its own type choice (Set Geometry Nodes Input); Gamepad Stick's linked
         * Invert multiplies by -1 at runtime (0 x -1 = -0) where a constant one gives +0. */
        const bool accepted_row = accepted.count(name.substr(0, name.find(' ') + 1)) != 0;
        must_match_failures += accepted_row ? 0 : 1;
        auto &examples = fold_examples[accepted_row ? name + " (accepted row: reported only)" :
                                                      name];
        if (examples.size() < 2) {
          examples.push_back("world " + std::to_string(world_seed) + ": " +
                             diff::FirstDifference(a.final_world, b.final_world));
        }
      }
    }
  }
  report += "profiling on against off (must be 0): " + std::to_string(profiled_different) + " of " +
            std::to_string(profiled_compared) + " seeds differ\n";
  for (const std::string &example : profiled_examples) {
    report += "  " + example + "\n";
  }
  report += "folded constants against runtime reads: " + std::to_string(fold_different) + " of " +
            std::to_string(fold_compared) + " probe runs differ, " +
            std::to_string(fold_examples.size()) + " different settings\n";
  for (const auto &entry : fold_examples) {
    report += "  " + entry.first + "\n";
    for (const std::string &example : entry.second) {
      report += "    " + example + "\n";
    }
  }
  report += "compiled " + std::to_string(compiled_count) + " of " + std::to_string(count) +
            "; must-match failures " + std::to_string(must_match_failures) + "\n";
  report += "lean VM against the reference mode: " + std::to_string(lean_different) +
            " of " + std::to_string(lean_compared) + " seeds differ (must be 0)\n";
  for (const std::string &example : lean_examples) {
    report += "  " + example + "\n";
  }
  report += "memo on against memo off: " + std::to_string(register_different) + " of " +
            std::to_string(register_compared) + " seeds differ (must be 0)\n";
  for (const std::string &example : register_examples) {
    report += "  " + example + "\n";
  }
  /* Which families a tree batch refuses (the probes, one node each): input to the batch lists. */
  {
    std::map<std::string, std::pair<int, int>> by_family; /* refused, total */
    std::map<std::string, std::string> reason_of;
    for (Probe &probe : Probes()) {
      const std::string family = probe.name.substr(0, probe.name.find(' '));
      const LN_TreeBatchProfile &profile = probe.compiled.program->GetTreeBatchProfile();
      by_family[family].second++;
      if (!profile.eligible) {
        by_family[family].first++;
        reason_of[family] = profile.reason;
      }
    }
    report += "families a tree batch refuses (refused / settings): ";
    for (const auto &entry : by_family) {
      if (entry.second.first > 0) {
        report += entry.first + " " + std::to_string(entry.second.first) + "/" +
                  std::to_string(entry.second.second) + " [" + reason_of[entry.first] + "]; ";
      }
    }
    report += "\n";
  }
  size_t kind_count = 0;
  const LN_KindInfo *rows = LN_KindCatalog(&kind_count);
  size_t unreached = 0, rare = 0;
  std::string rare_names;
  for (size_t index = 0; index < kind_count; index++) {
    const auto found = ran.find(rows[index].name);
    if (found == ran.end()) {
      unreached++;
    }
    else if (found->second < 50) {
      rare++;
      rare_names += std::string(" ") + rows[index].name;
    }
  }
  report += "kinds reached " + std::to_string(ran.size()) + " of " + std::to_string(kind_count) +
            ", reached fewer than 50 runs: " + std::to_string(rare) + rare_names + "\n";
  std::printf("%s", report.c_str());
  if (const char *path = std::getenv("UPBGE_LN_DIFF_REPORT")) {
    std::ofstream(path) << report;
  }
  EXPECT_EQ(must_match_failures, 0u);
}

}  // namespace
