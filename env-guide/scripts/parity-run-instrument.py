#!/usr/bin/env python3
"""Prepare a NEW build tree; afterwards use the normal configure/make or Maven build.

filebench: upstream 1.5-alpha3 (requires flex/yacc). ycsb: repo script/RocksDB.
Use the same resulting binaries on both sides. No installed tool is modified.
"""
import argparse
import shutil
from pathlib import Path


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError("unsupported/already modified source near " + repr(old[:80]))
    return text.replace(old, new)


def filebench():
    # 128 sub-buckets per power of two, exact for <256 ns. The last bin is overflow.
    # Keep the slow path (formatting and emitting nonzero bins) OUT of the timing loop.
    yield "stats.h", [("OSPROF_BUCKET_NUMBER\t64", "OSPROF_BUCKET_NUMBER\t4096")]
    yield "flowop.c", [
        ("unsigned long long iii;\n\n\tiii = 1;" + """
	for (i = 0; i < OSPROF_BUCKET_NUMBER; i++) {
		if (ll_delay < iii)
			break;
		iii <<= 1;
	}""", """unsigned int shift = ll_delay < 256 ? 0 : 56 - __builtin_clzll(ll_delay);
	i = 128 * shift + (ll_delay >> shift);
	if (i >= OSPROF_BUCKET_NUMBER)
		i = OSPROF_BUCKET_NUMBER - 1;"""),
        ("ll_delay = (gethrtime() - threadflow->tf_stime);", """ll_delay = (gethrtime() - threadflow->tf_stime);
	/* One writer per actual flow instance. Drain in-flight updates at snap. */
	__atomic_store_n(&flowop->fo_parity_writing, 1, __ATOMIC_SEQ_CST);
	if (!__atomic_load_n(&filebench_shm->shm_parity_active, __ATOMIC_SEQ_CST)) {
		__atomic_store_n(&flowop->fo_parity_writing, 0, __ATOMIC_SEQ_CST);
		return;
	}"""),
        ("(void) ipc_mutex_unlock(&controlstats_lock);\n}",
         "(void) ipc_mutex_unlock(&controlstats_lock);\n"
         "\t__atomic_store_n(&flowop->fo_parity_writing, 0, __ATOMIC_SEQ_CST);\n}"),
    ]
    yield "flowop.h", [("pthread_mutex_t\tfo_lock;", "int fo_parity_writing;\n\tpthread_mutex_t\tfo_lock;")]
    yield "ipc.h", [("int\t\tlathist_enabled;", "int shm_parity_active;\n\tint\t\tlathist_enabled;")]
    yield "stats.c", [
        ("globalstats->fs_stime = gethrtime();", "globalstats->fs_stime = gethrtime();\n"
         "\t__atomic_store_n(&filebench_shm->shm_parity_active, 1, __ATOMIC_SEQ_CST);"),
        ("filebench_shm->shm_bequiet = 1;", "filebench_shm->shm_bequiet = 1;\n"
         "\t__atomic_store_n(&filebench_shm->shm_parity_active, 0, __ATOMIC_SEQ_CST);"),
        ("/* Roll up per-flowop into global stats */",
         "while (__atomic_load_n(&flowop->fo_parity_writing, __ATOMIC_SEQ_CST))\n"
         "\t\t\tsched_yield();\n\n\t\t/* Roll up per-flowop into global stats */"),
        ("\t\tchar histogram[1024];\n\t\tchar hist_reading[20];\n", ""),
        ("""			(void) sprintf(histogram, "\\t[ ");
			for (i = 0; i < OSPROF_BUCKET_NUMBER; i++) {
				(void) sprintf(hist_reading, "%lu ",
				flowop->fo_stats.fs_distribution[i]);
				(void) strcat(histogram, hist_reading);
			}
			(void) strcat(histogram, "]\\n");
			(void) strcat(str, histogram);""",
         """			if (flowop->fo_type == FLOW_TYPE_IO || flowop->fo_type == FLOW_TYPE_AIO)
				for (i = 0; i < OSPROF_BUCKET_NUMBER; i++)
					if (flowop->fo_stats.fs_distribution[i])
						printf("PARITY-HIST-v1 %s %d %lu\\n", flowop->fo_name,
						    i, flowop->fo_stats.fs_distribution[i]);
			(void) strcat(str, "\\n");"""),
    ]
    yield "parser_gram.y", [
        ('#include "stats.h"', '#include "stats.h"\n#include "cvars/mtwist/mtwist.h"'),
        ("\tinit_common();", """	{
		const char *base = getenv("PARITY_SEED");
		unsigned int seed;
		const char *role;
		if (!base || !*base)
			usage_exit(1, "parity binary requires PARITY_SEED");
		seed = strtoul(base, NULL, 10);
		if (mode == FB_MODE_MASTER)
			printf("PARITY-SEED-v1 %u\\n", seed);
		for (role = fbparams.procname; role && *role; role++)
			seed = 33U * seed + (unsigned char)*role;
		seed += (unsigned int)(fbparams.instance + 1);
		srand(seed);
		srand48(seed);
		mt_seed32new(seed);
	}
	init_common();"""),
    ]


def ycsb():
    root = "core/src/main/java/site/ycsb/"
    # Only the RNGs used by the four approved uniform CoreWorkload profiles.
    # Each client has a private reproducible stream independent of thread scheduling.
    yield root + "ClientThread.java", [
        ("  public void run() {", """  public void run() {
    long seed = Long.parseLong(props.getProperty("parity.seed"));
    ParityRandom.initialize(seed, threadid);
    System.out.println("PARITY-SEED-v1 " + seed + " " + threadid);""")]
    for name in ["generator/DiscreteGenerator", "generator/UniformLongGenerator", "RandomByteIterator"]:
        yield root + name + ".java", [
            ("import java.util.concurrent.ThreadLocalRandom;", "import site.ycsb.ParityRandom;"),
            ("ThreadLocalRandom.current()", "ParityRandom.current()")]
    yield root + "ParityRandom.java", [(None, """package site.ycsb;

import java.util.SplittableRandom;

public final class ParityRandom {
  private static final ThreadLocal<SplittableRandom> RNG = new ThreadLocal<>();
  private ParityRandom() { }
  public static void initialize(long seed, int client) {
    RNG.set(new SplittableRandom(seed + 0x9e3779b97f4a7c15L * client));
  }
  public static SplittableRandom current() {
    return RNG.get();
  }
}
""")]


def prepare(suite, src, dest):
    src, dest = src.resolve(strict=True), dest.resolve()
    if dest == src or src in dest.parents or dest.exists():
        raise ValueError("destination must be new and outside the original source")
    changes = {}
    for name, edits in (filebench if suite == "filebench" else ycsb)():
        original = src / name
        text = original.read_text() if original.exists() else None
        for old, new in edits:
            text = new if old is None and text is None else replace(text, old, new)
        changes[name] = text
    shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git", "target"))
    for name, text in changes.items():
        (dest / name).write_text(text)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suite", choices=["filebench", "ycsb"])
    parser.add_argument("source", type=Path)
    parser.add_argument("new_build_tree", type=Path)
    args = parser.parse_args()
    try:
        prepare(args.suite, args.source, args.new_build_tree)
    except (ValueError, OSError) as exc:
        parser.exit(1, "instrument: %s\n" % exc)
