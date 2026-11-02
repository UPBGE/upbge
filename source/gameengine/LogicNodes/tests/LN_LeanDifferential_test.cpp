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
                     const std::set<std::string> &exclude = {})
{
  Compiled compiled;
  TreeGen generator{seed, budget, depth};
  generator.Exclude(exclude);
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
  for (size_t index = 0; index < Probes().size(); index++) {
    const RunSummary &summary = ProbeSummary(index);
    EXPECT_TRUE(summary.failure.empty()) << Probes()[index].name << ": " << summary.failure;
    lines += "setting " + Probes()[index].name + " " + summary.hash +
             " runs=" + std::to_string(summary.tree_runs) + "\n";
  }
  const std::string verdict = CheckGolden("differential_v0", lines);
  EXPECT_EQ(verdict, "") << verdict;
}

/* The lean VM in Force mode (plan phase A16) against the old interpreter: the whole text of a run
 * (commands, applied world, every tree's state) is the same for each fixed seed and each setting
 * of the table, in the serial recording and, for the batch eligible trees, in a batch on a pool
 * (lean programs are made on the main thread and only read on the helpers). */
TEST(LN_LeanDifferential, TheLeanVMInForceModeMatchesTheOldInterpreter)
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

/* Every kind the trees reach ran at least 50 times (as many times as it appears in a program times
 * the tree runs), and the number of kinds not reached is printed: the generator's blind spots,
 * for the runtime phases that come after. */
TEST(LN_LeanDifferential, EveryKindTheTreesReachRanOften)
{
  std::map<std::string, uint64_t> ran;
  const auto add = [&](const RunSummary &summary) {
    for (const auto &kind : summary.kinds) {
      ran[kind.first] += uint64_t(kind.second) * summary.tree_runs;
    }
  };
  for (uint64_t seed = 1; seed <= NormalSeedCount(); seed++) {
    add(SeedSummary(seed));
  }
  for (size_t index = 0; index < Probes().size(); index++) {
    add(ProbeSummary(index));
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
    EXPECT_GE(entry.second, 50u) << entry.first << " ran only " << entry.second << " times";
  }
  std::printf("generated trees reach %zu of %zu kinds (%zu not reached)\n", ran.size(), count,
              unreached);
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
    /* Must match since plan phase A16: the lean VM (generic form) equals the old interpreter. */
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
                                  diff::FirstDifference(a.text, b.text));
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
        must_match_failures++;
        auto &examples = fold_examples[name];
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
  report += "lean VM (Force) against the old interpreter: " + std::to_string(lean_different) +
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
