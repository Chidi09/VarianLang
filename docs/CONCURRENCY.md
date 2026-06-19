# Concurrency: Tasks, Channels, Actors

Varian runs a cooperative, single-threaded green-thread scheduler — tasks yield
explicitly (or at blocking channel/actor operations), they are not preemptively
scheduled across OS threads.

## Tasks

```varian
fn worker(n) {
    print(n)
    task.yield()
    print(n + 10)
}

let t1 = task.spawn(worker, [1])
let t2 = task.spawn(worker, [2])

print(task.id(t1))
print(task.id(t2))
```

`task.spawn(fn_or_closure, args_array)` starts a new cooperative task. `task.yield()`
gives other tasks a turn. `task.spawn` correctly handles spawning a closure (one that
captured locals via `|...| {...}`), not just a plain named function.

## Channels

```varian
fn sender(ch) {
    ch <- 42
    ch <- 100
}
fn receiver(ch) {
    let a = <- ch
    print(a)
    let b = <- ch
    print(b)
}

let ch = task.channel(10)   // buffered, capacity 10
task.spawn(sender, [ch])
task.yield()
task.spawn(receiver, [ch])
```

- `ch <- value` sends. If the channel is full, the sending task **yields and retries**
  automatically (real backpressure — already built into `BC_CHAN_SEND`, not something
  you need to implement yourself).
- `<- ch` receives. If empty, the receiving task yields and retries. If the channel has
  been closed and is empty, it returns `nil` immediately instead of blocking forever —
  this is the mechanism to use for a clean worker-shutdown signal.

## Actors

```varian
actor Counter {
    count: int = 0,
    fn increment(self) {
        print("increment called")
    }
    fn get(self) -> int {
        return 42
    }
}

let c = Counter.spawn()
c.increment()
let val = c.get()
```

Actor methods take an explicit `self`, just like struct methods via `impl`. Calling a
method on an actor sends a message to its inbox and (cooperatively) waits for the
reply — it looks like a normal synchronous call but is implemented as async message
passing under the hood, so it's safe to call from multiple tasks without manual
locking.

## Background jobs (`queue.vn`)

```varian
// Cron: runs `handler` every `interval_ms`, forever
cron(200, || { print("tick") })

// Worker pool: N background workers pulling jobs off a shared channel
let pool = WorkerPool { ch: task.channel(1000), count: 0, workers: 0 }
pool.spawn(3)        // 3 workers
pool.submit(|| { print("processing job") })
```

`WorkerPool.submit()` is backed by a buffered channel, so it already has backpressure
for free (see Channels above) — submitting beyond the buffer's capacity just makes the
submitting task yield until a worker frees up space, it does not drop or error.
