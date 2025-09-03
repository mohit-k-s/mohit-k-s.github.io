+++
title = "Commmon Distributed Systems Lingo"
date = "2025-07-29"
description = "Distributed Systems Lingo"
tags = [
    "distributed systems",
]
+++

There are a few terms that keep popping up when you work with distributed systems. They're often thrown around casually, and it’s easy to nod along without fully understanding them. I’m collecting those here with simple definitions and examples. This list will evolve as I keep learning.

---
## Fencing Token

I was reading [V's blog](https://avi.im/blag/2024/s3-log/#:~:text=a%20concept%20of-,fencing%20tokens.,-I%E2%80%99ve%20left%20this) and he mentioned fencing token. This word was introduced in Martin's blog [here](https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html).

A **fencing token** is a number (usually monotonically increasing) that helps prevent **split-brain writes** or **duplicate actions** in distributed systems.

It’s commonly used in **distributed locks**. Here's why:

Let’s say two nodes both believe they hold a lock (due to clock drift or network delays). Without fencing, both might write to a shared resource. With fencing, each action is tagged with a **token** (say, 41 and 42). The system can reject the one with the lower token, assuming it’s stale.

> **Only the action with the highest fencing token is considered valid.**

This way, even if the lock coordination fails, the system can still reject outdated operations.

---

## Network Partition

A **network partition** is when a group of nodes in a distributed system can no longer talk to another group — not because the nodes have failed, but because something in the network is blocking communication between them.

Think of it as the system getting split into isolated islands.

> **Note:** A partition is about communication failure, not node failure.

### Examples:

- A node can talk to nodes A and B, but not to node C. That’s a network partition.
- Someone misconfigures firewall rules between two subnets. That’s a network partition.
- NACLs (Network Access Control Lists) are misconfigured to block traffic between nodes. 

---



