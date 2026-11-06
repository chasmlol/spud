# .oss — scripts (kind `SCRP`, version 2)

Layout: [`src/formats/oss.hpp`](../../src/formats/oss.hpp) (records with `static_assert`ed sizes are the source of truth). Written by the importer (`skytool import`, or `skytool script import` alone) to `<cache>/scripts/scripts.oss` from every `.pex` the load order can see, with loose files over archives, and the VMAD of every winning record. Read by the VM in `src/script`, whose behaviour is.

| Chunk | Contents |
|---|---|
| `SCRP` | `Script[]`, sorted by lower-case name |
| `VARS` | `Variable[]`: each script's object variables, in declaration order |
| `PROP` | `Property[]` |
| `STAT` | `State[]`: each script's states, the empty state among them |
| `FUNC` | `Function[]`: state functions, and the get and set functions of full properties |
| `SLOT` | `Slot[]`: a function's parameters, then its locals |
| `INST` | `Instruction[]` |
| `OPER` | `Operand[]` |
| `ATCH` | `Attachment[]`: scripts records attach to themselves, sorted by form ID, each form's in VMAD order |
| `APRP` | `AttachedProperty[]` |
| `AVAL` | `AttachedValue[]` |
| `FRAG` | `Fragment[]`: script fragments, sorted by form ID |
| `STRS` | string table |

Everything refers to other tables by index and to strings by offset into `STRS`. Strings keep their case; the VM interns them case-insensitively.

## Code

A function's instructions are a contiguous run of `INST`, with opcodes in PEX numbering. An instruction's operands are a run of `OPER`:

| Opcode | Operands |
|---|---|
| `call_method` | function name, object, destination, then the arguments |
| `call_parent` | function name, destination, then the arguments |
| `call_static` | script name, function name, destination, then the arguments |
| `prop_get` | property name, object, destination |
| `prop_set` | property name, object, value |
| `jmp` | target |
| `jmpt`, `jmpf` | value, target |
| others | as in PEX, 2.3 |

What the importer resolves:

- **Identifiers:** a `slot` for a parameter or local, a `member` for an object variable of the same script (an index into that script's own variables), `self`, `discard` for `::NoneVar`, `state` for `::State`. An identifier that is none of these stays a `name`, and reading it is the VM's "not successfully looked up" error.
- **Jumps:** PEX offsets are relative to the jump; here the target is an index into the function's instructions.
- **Argument counts:** PEX stores one before the variable arguments. Here `operand_count` gives it.

The PEX debug tables, doc strings, user names and compile times are dropped. Bad data in one `.pex` gives a warning and drops that file.

## Attachments

Each `Attachment` is one script in a record's VMAD: its name, status (3 means removed) and properties. An `AttachedProperty` is a run of `AttachedValue`, of length 1 unless `array` is set. Object values are form IDs resolved into the load order, with the alias index for a quest alias, or -1.

A quest alias's scripts are attachments on the quest with `alias` set; everything else has `alias` -1.

A `Fragment` is one fragment in a VMAD: a method on a script the same record attaches. By `kind`: a quest stage's (`index` the stage, `entry` the log entry), a topic info's begin and end, a package's begin, end and change, a scene's begin, end and phase (`index` the phase, `entry` 0 at its start, 1 at its end), and a perk entry point's.

## Versions

- 1: first version.
- 2: quest alias scripts and fragments. Older files aren't read: `skytool script import` rewrites them.
