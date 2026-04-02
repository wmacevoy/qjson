# QJSON Datalog

A fact language for humans, robots, and agents.  Precise. Load-bearing.
Five verbs, two freezes.  Everything is the database.

## Facts

A fact is a named QJSON object followed by a period.  The period
means "this is true."

```
is_parent: {parent: alice, child: bob}.
is_parent: {parent: bob, child: carol}.
is_parent: {parent: carol, child: dave}.
is_reading: {patient: alice, glucose: 142M, time: 1710000000N}.
```

Bulk:

```
facts:
  is_parent: {parent: alice, child: bob}
  is_parent: {parent: bob, child: carol}
  is_parent: {parent: carol, child: dave}
```

Values are QJSON: `M` for exact decimal, `N` for big integer, `L` for
big float, `0j` for blobs, `?X` for variables.  Valid JSON is valid QJSON.

## Rules

A rule is a fact with a `where` clause.  It derives new facts from
existing ones.  It's in the database like everything else.

```
is_grandparent: {grandparent: ?GP, grandchild: ?GC}
  where is_parent: {parent: ?GP, child: ?P}
    and is_parent: {parent: ?P, child: ?GC}.
```

`?P` appears in both patterns — that's a join.  The engine figures it out.

Rules can use arithmetic.  Type widening selects exact base-10 or
base-2 automatically:

```
has_total: {name: ?N, total: ?T}
  where has_item: {name: ?N, unit_price: ?U, qty: ?Q}
    and ?T = ?U * ?Q.
```

`0.1M + 0.2M = 0.3M`.  Not `0.30000000000000004`.

Boolean conditions: `and`, `or`, `not`, parentheses.

```
is_eligible: {patient: ?P}
  where is_patient: {name: ?P, age: ?A}
    and ?A >= 18
    and not is_excluded: {patient: ?P}.
```

## Queries

```
select is_grandparent: {grandparent: ?GP, grandchild: carol}
```

Put concrete values in the pattern to filter.  Variables return
all matches.

## Signals

A signal is a transient fact.  It enters the brain, triggers
reactions, and is never stored.

```
signal pump_reading: {patient: alice, glucose: 280M, time: 1710000600N}
```

Signals are both input and output.  They come from the world
(a sensor, an API, a user action) and go back out (an alert,
a command, a UI update).  The host decides what outbound signals
mean.

## Triggers

`when` reacts to state changes.  Three events: `signal`, `assert`,
`retract`.  Nothing else happens in a fact-based system.

```
when signal pump_reading: {patient: ?P, glucose: ?G, time: ?T}:
  assert is_reading: {patient: ?P, glucose: ?G, time: ?T}
```

A `where` clause adds conditions and computation:

```
when signal pump_reading: {patient: ?P, glucose: ?G, time: ?T}
  where is_reading: {patient: ?P, time: ?Old}
    and ?Old < ?T:
  retract is_reading: {patient: ?P, time: ?Old}
  assert is_reading: {patient: ?P, glucose: ?G, time: ?T}
```

Triggers cascade.  A signal triggers a `when`, which asserts a
fact, which triggers another `when`, which signals outward.
Until fixpoint — no new actions to take.

```
// Sensor arrives
when signal pump_reading: {patient: ?P, glucose: ?G, time: ?T}:
  assert is_reading: {patient: ?P, glucose: ?G, time: ?T}

// Reading stored → check thresholds
when assert is_reading: {patient: ?P, glucose: ?G}
  where ?G > 250M:
  signal alert: {patient: ?P, glucose: ?G, level: critical}

// Alert → the host handles it (SMS, LED, React component, log)
```

The brain signals `alert`.  It doesn't know what an SMS is.  The
host watches for outbound signals and acts.  That's the embedding.

## Retract

Forget a fact.  Pattern matching selects what to remove.

```
retract is_reading: {patient: alice, time: ?Old}
  where ?Old < 1710000000N
```

## Freeze

### mineralize

Freeze a rule into concrete facts.  Evaluate the view, replace
the definition with its results.  Immutable.  Auditable.

```
mineralize(is_alert_rule)
```

Before: a live rule that re-evaluates.  After: frozen facts
that document what the rule produced.  An FDA auditor sees
facts, not code.

### fossilize

Freeze everything.  All rules become facts.  The database becomes
a snapshot — a fossil of what the brain knew at that moment.

```
fossilize
```

Signals still flow through fossilized rules.  The rules can't
change, but the brain still reacts.  The skull protects the brain.

## The whole language

```
// Database (facts + rules, all just assertions)
name: {pattern}.                              fact
name: {pattern} where condition.              rule
facts:                                        bulk facts
  name: {data}
  name: {data}

// Triggers (the nervous system)
when signal pattern where condition:          on transient input
  assert|retract|signal action
when assert pattern where condition:          on fact added
  assert|retract|signal action
when retract pattern where condition:         on fact removed
  assert|retract|signal action

// Operations
signal name: {data}                           transient in/out
retract name: {pattern} where condition       forget
select name: {pattern}                        query
mineralize(name)                              freeze one
fossilize                                     freeze all
```

Five verbs: assert (`.`), retract, signal, select, when.
Two freezes: mineralize, fossilize.
One data format: QJSON (exact decimals, big integers, blobs, variables).

## Embedding

The brain doesn't know about the outside world.  The host provides
the body.

```
Signals in:    host → signal pump_reading: {...}
Brain reacts:  when signal → assert → when assert → signal alert
Signals out:   signal alert: {...} → host handles it
```

The host is whatever runs the brain:

| Host | Signals in | Signals out |
|------|-----------|-------------|
| Browser (WASM) | fetch → signal | signal → setState |
| CLI (qjq) | stdin → signal | signal → stdout |
| Embedded (C) | GPIO → signal | signal → GPIO |
| Server (Python) | HTTP → signal | signal → response |

libqjson is the brain.  The host is the body.  `signal` is the
nerve.  The language is the same everywhere.

## Example: insulin pump monitor

```
// Configuration (facts)
config: {patient: alice, max_iob: 5.0M, target: {low: 80M, high: 120M}}.

// Alert rules (freeze after validation)
is_high: {patient: ?P, glucose: ?G, level: critical}
  where is_reading: {patient: ?P, glucose: ?G}
    and ?G > 250M.

is_low: {patient: ?P, glucose: ?G, level: critical}
  where is_reading: {patient: ?P, glucose: ?G}
    and ?G < 70M.

is_over_max_iob: {patient: ?P, iob: ?IOB, max: ?MAX}
  where is_reading: {patient: ?P, iob: ?IOB}
    and config: {patient: ?P, max_iob: ?MAX}
    and ?IOB > ?MAX.

// Freeze the rules — immutable, auditable
mineralize(is_high)
mineralize(is_low)
mineralize(is_over_max_iob)

// Nervous system (triggers)
when signal pump_reading: {patient: ?P, glucose: ?G, iob: ?IOB, time: ?T}:
  assert is_reading: {patient: ?P, glucose: ?G, iob: ?IOB, time: ?T}

when assert is_reading: {patient: ?P, glucose: ?G}
  where is_high: {patient: ?P}
    or is_low: {patient: ?P}
    or is_over_max_iob: {patient: ?P}:
  signal alert: {patient: ?P, glucose: ?G, level: critical}

// Life
signal pump_reading: {patient: alice, glucose: 280M, iob: 3.2M, time: 1710000600N}
// → assert is_reading
// → is_high matches (280M > 250M)
// → signal alert
// → host sends SMS, updates dashboard, logs to audit trail
```

A nurse reads the rules.  An agent writes them.  An auditor
verifies the frozen facts.  The pump stays safe.
