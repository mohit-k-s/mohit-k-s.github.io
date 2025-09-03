+++
title = "169220804: Yet Another Magic Number"
date = "2025-08-10"
description = "Arrays in js"
tags = [
    "v8_internals",
]
+++
I was poking around with Node.js arrays, trying to see how far I could push them
Here’s the simple code I ran:
```js
const at = [] 

for(let i =0 ; i < 1e9 ; ++i) {
    at.push(true)
}

```
This is just a standard array, and of course it fails — there’s no way my system has ~7.5 GB available just for this process.
But it fails with an interesting error:

```log
#
# Fatal error in , line 0
# Fatal JavaScript invalid size error 169220804 (see crbug.com/1201626)
#
#
#
#FailureMessage Object: 0x16bc59878
----- Native stack trace -----

 1: 0x104301560 node::NodePlatform::GetStackTracePrinter()::$_0::__invoke() [/Users/mohit/.nvm/versions/node/v20.19.0/bin/node]
  ...
```
What’s that number *169220804*?

You might think: “Maybe if I just create an array of length *169220803*, it’ll work?” Nope !

Each node version ships with a particular v8, here is my config 
```bash
➜ node -p process.versions
{
  node: '20.19.0',
  v8: '11.3.244.8-node.26',
  ...
}
```

So, I checked that branch of the V8 source.
kMaxSize is defined [here](https://chromium.googlesource.com/v8/v8/+/refs/heads/11.3.244/src/objects/fixed-array.h#91).

That’s the max size in bytes, but the max number of elements comes from [kMaxLength](https://chromium.googlesource.com/v8/v8/+/refs/heads/11.3.244/src/objects/fixed-array.h#207) which works out to *134,217,728* elements.

So in theory, creating an array of that length is fine.
But my program is using .push().


## The growth rule
When .push() needs more capacity, V8 allocates a new backing store at ~1.5× the old size and copies the elements over.

```cpp
  // file src/builtins/growable-fixed-array-gen.cc
  // Growth rate is analog to JSObject::NewElementsCapacity:
  // new_capacity = (current_capacity + (current_capacity >> 1)) + 16.
  const TNode<IntPtrT> new_capacity =
      IntPtrAdd(IntPtrAdd(current_capacity, WordShr(current_capacity, 1)),
                IntPtrConstant(16));
```
## Finding the actual limit
At first , I didn't know the above formula and just wanted to find the point at which i start seeing these errors so , I wrote a quick binary search to find the exact point where the above error appears.

The magic number turned out to be *112,813,859* elements.

If we plug that into the growth formula:

```js
new_capacity = 112_813_859 + floor(112_813_859 / 2) + 16
             = 112_813_859 + 56_406_929 + 16
             = 169_220_804
```
Ahh, that exact same number.
It all makes sense now. \
I tried running this in later node versions , the program always breaks at that limit. 

## Takeaways

1. While the theoritical limit is way higher for array length, their maximum size and growth behavior are constrained by V8’s internal constants like `kMaxSize` and `kMaxLength`.

2. Understanding these engine-level limits is essential when working with extremely large arrays or developing performance-critical applications that rely on dynamic array growth.

3. For predictable and efficient handling of large numeric datasets, consider using typed arrays (`Uint8Array`, `Float64Array`, etc.), which have fixed size and avoid the overhead and limits of dynamic arrays.