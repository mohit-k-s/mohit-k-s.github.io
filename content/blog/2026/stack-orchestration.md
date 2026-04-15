+++
title = "The Reservation Pattern: Claim First, Build Later"
date = "2026-04-15"
category = "tech"
tags = [
    "distributed-systems",
    "system-design",
]
+++

I was thinking about what happens when you double-click that "Create" button on AWS. Or when two teammates simultaneously try to spin up environments that share the same VPC.

Seems simple — just check if the resource already exists. But it's one of those deceptively deep distributed systems problems.

## The Double-Click Problem

A user clicks "Create Infrastructure" and their browser fires two identical API requests.

Naive solution: before creating a resource, check if it already exists by name. If AWS returns `AlreadyExistsException`, catch it and move on.

**This works.** If every resource has a natural unique identifier (like a VPC name), the cloud provider's own uniqueness constraint is your deduplication layer.

But what if you're creating multiple resources?

## Multiple Interdependent Resources

A single "create my environment" request might translate to five resources:

```
VPC -> Subnet -> Security Group -> EC2 Instance -> DNS Record
```

Each depends on the one before it. Now the interesting failures show up:

**Partial failure.** You create the VPC and Subnet, then crash while creating the Security Group. User retries. You need to *resume from where you left off* — not start over, not create a second VPC.

**Concurrent overlapping requests.** Two stacks want some of the same resources:

```
Stack X: [VPC-1, Subnet-1, SG-1, EC2-A, DNS-A]
Stack Y: [VPC-1, Subnet-1, SG-1, EC2-B, DNS-B]
                ^^^    ^^^   ^^^
            overlapping resources
```

Who wins? What happens to the resources the loser already created?

## The Naive Approach: Build First, Ask Questions Later

Create resources one by one, check for conflicts as you go.

```
for each resource in stack:
    if resource exists and belongs to another stack:
        → conflict, roll back everything
    else:
        → create it, claim ownership
```

This has a fundamental flaw. By the time you detect a conflict at step 3, you've already created real infrastructure at steps 1 and 2. Now you have to tear it down. If *that* teardown fails? Orphaned resources floating in the cloud, owned by a stack that no longer exists.

You're doing expensive, hard-to-reverse work before you even know if the operation is valid.

## Claim First, Build Later

Here's the key insight: **the conflict detection problem and the resource creation problem are completely independent.** You don't need to create anything to know if there's a conflict — just check if any requested resource is already claimed by another stack.

This gives you a three-phase architecture:

### Phase 1: Admission Gate (Synchronous)

When a stack creation request comes in, don't create any infrastructure. Write every requested resource name into an ownership table with a **primary key on `resource_name`**.

```
Request: Create Stack X with [VPC-1, Subnet-1, SG-1, EC2-A, DNS-A]

→ INSERT (resource_name="VPC-1",    stack_id=X, status=RESERVED)
→ INSERT (resource_name="Subnet-1", stack_id=X, status=RESERVED)
→ INSERT (resource_name="SG-1",     stack_id=X, status=RESERVED)
→ INSERT (resource_name="EC2-A",    stack_id=X, status=RESERVED)
→ INSERT (resource_name="DNS-A",    stack_id=X, status=RESERVED)
```

If any insert fails (primary key violation), the resource is already claimed. Mark this stack as **INVALID**. No infrastructure was touched. No rollback needed.

Worst case O(n) — one check per resource — and each check is a single atomic database write. Race conditions are impossible because the database constraint handles concurrency for you.

### Phase 2: Creator Scheduler (Asynchronous)

A background scheduler polls for valid stacks with resources in `RESERVED` status:

1. Look at the dependency DAG
2. Find all resources whose dependencies are already `CREATED`
3. Create that batch in parallel
4. Mark them as `CREATED`, come back for the next wave

```
Tick 1: VPC-1 has no dependencies → create it
Tick 2: Subnet-1 depends on VPC-1 (done) → create it
        SG-1 depends on VPC-1 (done) → create it     ← parallel!
Tick 3: EC2-A depends on Subnet-1 + SG-1 (done) → create it
Tick 4: DNS-A depends on EC2-A (done) → create it
```

The scheduler doesn't worry about conflicts — admission already handled that. It doesn't worry about ordering — the DAG tells it what's ready. It just picks up work and executes it.

If a creation fails, the resource stays `RESERVED`. Scheduler retries on the next tick. Too many retries? Mark the stack as `FAILED`.

**Resumability comes for free.** System crashes after creating VPC and Subnet? Scheduler picks up on the next tick, sees those are `CREATED`, continues from the Security Group.

### Phase 3: Cleanup Scheduler (Asynchronous)

A second scheduler polls for stacks marked `INVALID` or `FAILED`:

1. Find all resources owned by this stack
2. If a resource was actually created (`CREATED`), delete it from the cloud provider
3. Delete succeeds → remove the ownership claim
4. Delete fails → mark as `ORPHANED`

A garbage collection sweep periodically retries `ORPHANED` resources until they're gone.

## The Data Model

```
Stack:
  stack_id        : UUID
  status          : VALIDATING | VALID | INVALID | IN_PROGRESS | COMPLETE | FAILED
  created_at      : timestamp

Resource Ownership:
  resource_name   : string    ← PRIMARY KEY (enforces uniqueness)
  stack_id        : UUID
  status          : RESERVED | CREATING | CREATED | DELETING | ORPHANED
  claimed_at      : timestamp

Execution Log:
  stack_id        : UUID
  resource_name   : string
  action          : RESERVE | CREATE | DELETE
  result          : SUCCESS | FAILED | CONFLICT
  timestamp       : timestamp
```

## Why This Works

Each phase only cares about one thing:

| Phase | Responsible For | Doesn't Care About |
|-------|----------------|-------------------|
| Admission | Conflict detection | Resource creation, ordering, rollback |
| Creator | Building infrastructure | Conflicts, validation |
| Cleanup | Tearing down failures | Conflicts, creation order |

Admission is just database writes. Creator is just a DAG walker. Cleanup is just a list of things to delete. Compare this to the "build first" approach where a single code path handles creation, conflict detection, rollback, and orphan cleanup — all interleaved, all compounding on each other.

## This Isn't New

This separation shows up everywhere:

- **CloudFormation** separates change set creation from execution
- **Kubernetes** has the API server (admission + etcd writes) and the kubelet (container creation) as separate processes
- **Database engines** use write-ahead logs — decide what to do, write it down, then execute
- **Airline booking** reserves seats before issuing tickets

The pattern is always the same: make the decision durable before you make it real. Undoing a row in a table is trivial. Undoing a half-provisioned EC2 instance with an attached EBS volume and a security group is not.

## Wrapping Up

What started as "how do I handle a double-click" turned into a distributed systems design problem. But the core insight is simple:

> Don't let the system create anything until it has already decided, atomically, that every resource is available.

Separate planning from execution. Let the database handle concurrency. Make everything else async and idempotent.
