# Running experiments

The project specification expects your experiments to be **reproducible**: a
grader should be able to regenerate the raw data behind every figure without
editing your source code. `build/kv_bench` exists to make that easy for the
baseline. Every parameter is a command-line flag and the output is one CSV row,
so an experiment is a shell loop and a figure is a plot of a CSV file.

This document describes the tool that ships with the baseline. The experiments
themselves are yours to design — see the specification's "Experimental
Evaluation" section for what each one has to state (research question,
hypothesis, baseline and method).

---

## The benchmark tool

```sh
make
./build/kv_bench --help
```

`kv_bench` runs three phases against a freshly created database:

1. **Load** — writes `--num-keys` distinct keys in a random order. Random order
   matters: ascending keys would be an unrealistically friendly case for the
   memtable.
2. **Point queries** — `--num-queries` lookups. `--miss-fraction` aims some of
   them at keys that were never written (these are the ones the Bloom filters
   act on), and `--zipf-skew` makes the rest skewed rather than uniform.
3. **Scans** — `--num-scans` range queries of `--scan-length` entries each,
   expressed as `Seek` plus `Next`, skipped when `--scan-length` is 0.

Buffer pool statistics are reset between phases, so the `point_*` and `scan_*`
columns describe their own phase only.

### Flags

**Workload**

| Flag | Meaning | Default |
| --- | --- | --- |
| `--db PATH` | database directory (deleted first, and afterwards unless `--keep-db`) | `bench_db` |
| `--num-keys N` | distinct keys written in the load phase | 200000 |
| `--num-queries N` | point lookups | 20000 |
| `--miss-fraction F` | fraction of lookups aimed at absent keys | 0 |
| `--zipf-skew S` | 0 = uniform; try 0.99 for a typically skewed workload | 0 |
| `--scan-length N` | entries per range scan; 0 disables the scan phase | 0 |
| `--num-scans N` | number of range scans | 100 |
| `--seed N` | RNG seed | 42 |

**System under test**

| Flag | Meaning | Default |
| --- | --- | --- |
| `--memtable-bytes N` | memtable capacity | 1048576 |
| `--buffer-pool-pages N` | buffer pool capacity, in 4 KiB pages | 1024 |
| `--bloom-bits N` | Bloom filter bits per entry; 0 disables filters | 10 |
| `--size-ratio N` | LSM size ratio *T* | 2 |
| `--no-direct-io` | use buffered I/O instead of `O_DIRECT` | off |

**Output**

| Flag | Meaning |
| --- | --- |
| `--label NAME` | the value of the `label` column — use it to name the variant |
| `--csv-header` | print the header line (pass it only on the first run) |
| `--keep-db` | leave the database directory in place afterwards |

### CSV columns

The configuration columns echo the flags, so each row records the settings that
produced it. The measured columns are:

| Column | Meaning |
| --- | --- |
| `direct_io` | 1 if `O_DIRECT` was actually granted — **check this** (see below) |
| `load_seconds`, `put_per_second` | load phase throughput |
| `query_seconds`, `get_per_second` | point query phase throughput |
| `queries_found` | lookups that returned a value; a sanity check on `miss_fraction` |
| `scan_seconds`, `entries_scanned` | scan phase |
| `flushes`, `compactions` | memtable flushes and level merges during the load |
| `num_ssts`, `num_levels` | shape of the tree after the load |
| `sst_entries` | entries stored on disk, tombstones and obsolete versions included — against the number of live keys, this is space amplification |
| `point_hits`, `point_misses`, `point_evictions` | buffer pool during the query phase; a miss is one 4 KiB `pread` |
| `scan_hits`, `scan_misses` | buffer pool during the scan phase |

### Examples

One run, with a header:

```sh
./build/kv_bench --csv-header --num-keys 1000000 --num-queries 100000
```

A sweep — the shape most experiments take:

```sh
./build/kv_bench --csv-header --buffer-pool-pages 64  --label pool64  > results.csv
for pages in 128 256 512 1024 2048 4096; do
  ./build/kv_bench --buffer-pool-pages $pages --label pool$pages     >> results.csv
done
```

Comparing a baseline against a variant of your own, holding everything else
fixed:

```sh
for bits in 0 2 4 6 8 10 12 16; do
  ./build/kv_bench --bloom-bits $bits --miss-fraction 1.0 \
                   --label "bloom$bits" >> filter_sweep.csv
done
```

---

## Rules worth following

**Change one thing at a time.** Every row should differ from its baseline in one
flag. That is what lets you attribute the difference to the thing you changed.

**Check `direct_io`.** If the column reads 0, the filesystem refused `O_DIRECT`
and the OS page cache is acting as a second, invisible buffer pool — which makes
any statement about I/O volume or cache behaviour meaningless. `tmpfs` and some
container and network filesystems do this. Run on a real local disk. The library
reports the same thing through `Database::uses_direct_io()`.

**Warm state deliberately.** A cold buffer pool and a warm one measure different
things. `kv_bench` starts each query phase with whatever the load phase left in
the pool; if you want a cold measurement, say so in your method, and consider
reopening the database (which clears the pool) first.

**Record the machine.** CPU, RAM, and which device the database directory is on.
Two of the three dominate most of these numbers.

**Repeat.** Run each configuration at least three times, with different `--seed`
values, and report the variation. A 5% difference from a single run is noise.

**Do not benchmark-fish.** If a technique loses on some workload regime, that
result belongs in the report. The specification is explicit about this, and a
well-explained negative result is worth more than a flattering one.

---

## Adding your own experiments

When you build something the flags above cannot express — a new workload shape, a
new statistic, a targeted microbenchmark — add it as a small tool in `tools/`.
The Makefile compiles every `.cpp` in `src/` into the library and links
`tools/*.cpp` into `build/kv_bench`, so if you want a *separate* binary, add a
rule for it rather than a second `main()`.

Whatever you add, keep the two properties that make the results usable: every
parameter comes from the command line, and the output is machine-readable. Put
the exact commands in your report or in a script under `experiments/`, so that
the numbers can be regenerated.
