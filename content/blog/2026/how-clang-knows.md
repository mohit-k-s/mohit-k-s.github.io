+++
title = "How Clang Knows You Forgot <math.h>"
date = "2026-10-15"
category = "tech"
tags = [
    "c",
    "compilers",
    "clang",
    "llvm",
]
+++

##### It's been a while. 

Recently I started learning C again and i was writing a small C program to calculate normal distribution intervals and intentionally omitted `<math.h>` just to see what would happen:

```c
#include <stdio.h>

int main(void) {
    printf("between -1.96 and 1.96 is: %g\n", erf(1.96 * sqrt(1/2.)));
    return 0;
}
```

I expected a generic *"unknown function"* or link error. Instead, Clang hit me with this:

```text
hello.c:6:36: error: call to undeclared library function 'erf' with type 'double (double)'; ISO C99 and later do not support implicit function declarations [-Wimplicit-function-declaration]
hello.c:6:36: note: include the header <math.h> or explicitly provide a declaration for 'erf'
hello.c:6:45: error: call to undeclared library function 'sqrt' with type 'double (double)'; ISO C99 and later do not support implicit function declarations [-Wimplicit-function-declaration]
hello.c:6:45: note: include the header <math.h> or explicitly provide a declaration for 'sqrt'
```

Wait.

How does the compiler know `erf` takes a `double` and returns a `double`? How does it know `sqrt` belongs to `<math.h>` when `<math.h>` was never mentioned anywhere in the file?

A normal person would just add `#include <math.h>` and move on with their life.

I am not that person. Everything becomes a rabbit hole for me. I wanted to understand the exact mechanics of how the compiler knows this without the header, so I asked Gemini to dig around the Clang/LLVM source code and find where this is actually hardcoded.

## 1. The Compiler's Secret Table

It turns out modern compilers don't just parse C syntax—they have an internal database of the C standard library baked right into the compiler binary.

In Clang, this database historically lives in [`Builtins.def`](https://github.com/llvm/llvm-project/blob/llvmorg-18.1.8/clang/include/clang/Basic/Builtins.def#L238):

```c
BUILTIN(__builtin_erf , "dd", "Fne")
BUILTIN(__builtin_sqrt, "dd", "Fne")
```

At first glance, `"dd"` looks like gibberish, but it's Clang's internal type encoding shorthand:
* First letter = return type (`d` = `double`)
* Subsequent letters = argument types (`d` = `double`)
* Attribute `"F"` = this is a standard library function that also exists *without* the `__builtin_` prefix
* Attribute `"n"` = nothrow (won't throw C++ exceptions)
* Attribute `"e"` = pure/const (no side-effects when floating-point exceptions are ignored)

And the required header name (`math.h`) is registered alongside it in Clang's header descriptors.

## 2. Why is this an Error and Not Just a Warning?

Back in C89, if you called a function without declaring it, the compiler shrugged and assumed it returned an `int`:

```c
int erf(); // C89 implicit declaration assumption
```

On modern 64-bit architectures, this is a recipe for silent, catastrophic bugs:
* An `int` return value is passed in a general-purpose integer register (`%rax` on x86-64, `x0` on ARM64).
* A `double` return value is passed in a floating-point/vector register (`%xmm0` on x86-64, `d0` on ARM64).

If the caller expects the result in an integer register while `libm` placed it in a floating-point register, you end up reading garbage memory.

Starting with ISO C99, implicit function declarations were deprecated, and modern Clang treats `-Wimplicit-function-declaration` as a **hard error** by default.

## 3. The Diagnostic Engine

When Clang parses an identifier like `erf(...)` and can't find its declaration in scope, it doesn't immediately crash or emit a generic error. It queries its built-in catalog:

```cpp
// Inside clang/lib/Sema/SemaDecl.cpp
if (Context.BuiltinInfo.isLibFunction(BuiltinID)) {
    const char *Header = Context.BuiltinInfo.getHeaderName(BuiltinID);
    Diag(Loc, diag::ext_implicit_lib_function_decl_c99) << ...;
    if (Header)
        Diag(Loc, diag::note_include_header_or_declare) << Header;
}
```

The error message and note you see are defined in `DiagnosticSemaKinds.td`:

```tablegen
def ext_implicit_lib_function_decl_c99 : ExtWarn<
  "call to undeclared library function '%0' with type %1; ISO C99 and later "
  "do not support implicit function declarations">,
  InGroup<ImplicitFunctionDeclare>, DefaultError;

def note_include_header_or_declare : Note<
  "include the header <%0> or explicitly provide a declaration for '%1'">;
```

## 4. How It's Stored in Memory: Array or Map?

Once you realize the compiler has this catalog, the next natural question is: *how is this actually loaded in memory? Does Clang parse a table from disk on startup, or search a giant hash map every time you type a word?*

It turns out to be an elegant combination of both:

### The Database: A Static Array in `.rodata`
The metadata is **not** parsed or loaded dynamically. It is defined as a compile-time `constexpr static` array (`BuiltinInfos[]`):

```cpp
// clang/lib/Basic/Builtins.cpp
static constexpr Builtin::Info BuiltinInfos[] = {
    // ...
    // [238] = { NameOffset, TypeOffset, AttributesOffset, HeaderDesc::MATH_H } // erf
};
```

Because it is compile-time constant data, it gets baked directly into the **`.rodata` (read-only data)** segment of the `clang` binary on disk. 

When you run `clang hello.c`, the OS kernel memory-maps the binary into RAM. Zero parsing, zero disk reads, zero heap allocations. It is already in memory before your code is even touched. Accessing metadata by ID is an instant $O(1)$ array index.

### The Lookup: The `IdentifierTable` Tag
An array is great if you already have the integer ID. But when the compiler lexes your code, it starts with the string `"erf"`. Doing a linear scan over thousands of builtins for every single word would make compilation crawl.

Instead, Clang uses its central **`IdentifierTable`** (a string-interning hash table). When Clang initializes, it loops through the builtins once and tags each name inside the hash table with its integer ID:

```cpp
void Builtin::Context::initializeBuiltins(IdentifierTable &Table, const LangOptions &LangOpts) {
    for (unsigned id = 1; id < NumBuiltins; ++id) {
        IdentifierInfo &II = Table.get(getName(id)); 
        II.setBuiltinID(id); // Stamps the integer ID onto the hash table entry
    }
}
```

Inside the `IdentifierInfo` struct for `"erf"`, there is a tiny integer field: `unsigned ObjCOrBuiltinID`.

### The Lifecycle
When your file compiles, the whole chain runs in fractions of a microsecond:

```text
Source Code: erf(...)
     │
     ▼
1. Lexer hashes token "erf" into IdentifierTable
     │  (O(1) string lookup)
     ▼
2. IdentifierInfo already has integer tag set:
     │  II->getBuiltinID() -> returns 238
     ▼
3. Sema detects no user prototype, checks ID != 0
     │  (O(1) integer check)
     ▼
4. Direct array index:
     │  BuiltinInfos[238] -> signature "dd", header <math.h>
     ▼
5. Diagnostic emitted!
```

By separating the **data** (a flat, immutable array in `.rodata`) from the **string index** (a cached integer tag in the lexer's hash table), both steps are pure $O(1)$ memory reads.

## 5. Even the Rabbit Hole Had a Rabbit Hole

When Gemini pulled up the links to check the definitions, even the rabbit hole had its own rabbit hole: the links on LLVM's `main` branch seemed empty. Recent commits had stripped almost everything out of `Builtins.def`.

It turns out LLVM is in the middle of migrating all built-in definitions from old C-preprocessor macros to LLVM's TableGen format ([PR #68324](https://github.com/llvm/llvm-project/pull/68324)). Now they live in declarative `.td` files:

```tablegen
def ErfF128 : Builtin {
  let Spellings = ["__builtin_erff128"];
  let Attributes = [FunctionWithBuiltinPrefix, NoThrow, ConstIgnoringErrnoAndExceptions];
  let Prototype = "__float128(__float128)";
}
```

## The "Rabbit Hole" Tax

Sometimes I feel like I learn things way too slowly because I can't resist pulling on every loose thread. What started as a 2-minute C exercise turned into a whole session with an AI digging through LLVM compiler source code, CPU calling conventions, and TableGen refactors.

It's easy to feel frustrated by that kind of detour. But the more I do this, the more I realize: **I enjoy this more than just moving on**

<mark> Follow the rabbit hole once in a while. </mark>
