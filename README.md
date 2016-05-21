# rcu_ptr

## Introduction
Read-copy-update pointer (`rcu_ptr`) is a special smart pointer which can be used to exchange data between threads.
Read-copy-update (RCU) is a general synchronization mechanism, which is similar to readers-writers lock.
It allows extremely low overhead for reads. However, RCU updates can be expensive, as they must leave the old versions of the data structure in place to accommodate pre-existing readers \[[1][1], [2][2]\].
RCU is useful when you have several readers and few writers.
Depending on the size of the data you want to update, writes can be really slow, since they need to copy.
Therefore, it's worth to do measurments and analyze the characteristics of the inputs and environment of your system.

`rcu_ptr` implements the read-copy-update mechanism by wrapping a `std::shared_ptr` (in a way, it has some similarity to `std::weak_ptr`). 

## atomic_shared_ptr
`rcu_ptr` relies on the free [atomic_...](http://en.cppreference.com/w/cpp/memory/shared_ptr/atomic) function overloads for `std::shared_ptr`. Would be nice to use an [atomic_shared_ptr](http://en.cppreference.com/w/cpp/experimental/atomic_shared_ptr), but currently that is still in experimental phase.
We use atomic shared_ptr operations which are implemented in terms of a spin-lock (most probably that's how it is implemented in the currently available standard libraries).
Having a lock-free atomic_shared_ptr would be really benefitial. However, implementing a lock-free atomic_shared_ptr in a portable way can have extreme difficulties \[[3][3]\]. Thought it might be easier on architectures, where we have double word CAS operations.

## Why do we need `rcu_ptr`?
Imagine we have a collection and several reader and some writer threads on it.
It is a common mistake by some programmers to hold a lock until the collection is iterated on the reader thread.
Example

```c++
class X {
    std::vector<int> v;
    mutable std::mutex m;
public:
    int sum() const { // read operation
        std::lock_guard<std::mutex> lock{m};
        return std::accumulate(v.begin(), v.end(), 0);
    }
    void add(int i) { // write operation
        std::lock_guard<std::mutex> lock{m};
        v.push_back(i);
    }
};
```

The first idea to make it better is to have a shared_ptr and hold the lock only until that is copied by the reader or updated by the writer.
```c++
class X {
    std::shared_ptr<std::vector<int>> v;
    mutable std::mutex m;
public:
    X() : v(std::make_shared<std::vector<int>>()) {}
    int sum() const { // read operation
        std::shared_ptr<std::vector<int>> local_copy;
        {
            std::lock_guard<std::mutex> lock{m};
            local_copy = v;
        }
        return std::accumulate(local_copy->begin(), local_copy->end(), 0);
    }
    void add(int i) { // write operation
        std::shared_ptr<std::vector<int>> local_copy;
        {
            std::lock_guard<std::mutex> lock{m};
            local_copy = v;
        }
        local_copy->push_back(i);
        {
            std::lock_guard<std::mutex> lock{m};
            v = local_copy;
        }
    }
};
```
Now we have a race on the pointee itself during the write.
So we need to have a deep copy.
```c++
    void add(int i) { // write operation
        std::shared_ptr<std::vector<int>> local_copy;
        {
            std::lock_guard<std::mutex> lock{m};
            local_copy = v;
        }
        auto local_deep_copy = std::make_shared<std::vector<int>>(*local_copy);
        local_deep_copy->push_back(i);
        {
            std::lock_guard<std::mutex> lock{m};
            v = local_deep_copy;
        }
    }
```
The copy construction of the underlying data (vector<int>) is thread safe, since the copy ctor param is a const ref `const std::vector<int>&`.
Now, if there are two concurrent write operations than we might miss one update.
We'd need to check whether the other writer had done an update after the actual writer has loaded the local copy.
If it did then we should load the data again and try to do the update again.
This leads to the general idea of using an `atomic_compare_exchange` in a while loop.
So we could use an `atomic_shared_ptr` from C++17, but until then we have to settle for the free funtcion overloads for shared_ptr.

```c++
class X {
    std::shared_ptr<std::vector<int>> v;
public:
    X() : v(std::make_shared<std::vector<int>>()) {}
    int sum() const { // read operation
        auto local_copy = std::atomic_load(&v);
        return std::accumulate(local_copy->begin(), local_copy->end(), 0);
    }
    void add(int i) { // write operation
        auto local_copy = std::atomic_load(&v);
        auto exchange_result = false;
        while (!exchange_result) {
            // we need a deep copy
            auto local_deep_copy = std::make_shared<std::vector<int>>(
                    *local_copy);
            local_deep_copy->push_back(i);
            exchange_result =
                std::atomic_compare_exchange_strong(&v, &local_copy,
                                                    local_deep_copy);
        }
    }
};
```

Also (regarding the write operation), since we are already in a while loop we can use `atomic_compare_exchange_weak`.
That can result in a performance gain on some platforms.
See http://stackoverflow.com/questions/25199838/understanding-stdatomiccompare-exchange-weak-in-c11

But nothing stops an other programmer (e.g. a naive maintainer of the code years later) to add a new reader operation, like this:
```c++
    int another_sum() const {
        return std::accumulate(v->begin(), v->end(), 0);
    }
```
This is definetly a race condition and a problem. 
And this is the exact reason why `rcu_ptr` was created.
The goal is to provide a general higher level abstraction above `atomic_shared_ptr`.

```c++
class X {
    rcu_ptr<std::vector<int>> v;

public:
    X() { v.reset(std::make_shared<std::vector<int>>()); }
    int sum() const { // read operation
        std::shared_ptr<const std::vector<int>> local_copy = v.read();
        return std::accumulate(local_copy->begin(), local_copy->end(), 0);
    }
    void add(int i) { // write operation
        v.copy_update([i](std::vector<int>* copy) {
            copy->push_back(i);
        });
    }
};
```
The `read` method of `rcu_ptr` returns a `shared_ptr<const T>` by value, therefore it is thread safe.
The existence of the shared_ptr in the scope enforces that the read object will live at least until this read operation finishes.
By using the shared_ptr this way, we are free from ABA problems, see [Anthony Williams - Why do we need atomic_shared_ptr?](https://www.justsoftwaresolutions.co.uk/threading/why-do-we-need-atomic_shared_ptr.html).
The `reset` method receives a `const shared_ptr<T>&` with which we can overwrite the actual contained shared_ptr.
The `copy_update` method receives a lambda. This lambda is called whenever an update needs to be done, i.e. it will be called continuously until the update is successful.
The lambda receives a `T*` for the copy of the actual data.
We can modify the copy of the actual data inside the lambda.

## Usage
### Prerequisites

`rcu_ptr` depends on the features of the `C++14` standard, the tests were built with (GNU) `Make` (and with the GNU C++ toolchain) and dependent on the `ThreadSanitizer` introduced in GCC 4.8.

### Building the library

The library is header only: `rcu_ptr.hpp`.

### Running the tests

Tests are included to verify the concept and expected behaviour of the `rcu_ptr`. Each of these are in the subdirectories and building them is quite simple:
for example building and running the tests for container(s) would require the execution of the following steps:
```bash
cd container
make
./container
```

[1]: https://lwn.net/Articles/262464/
[2]: https://en.wikipedia.org/wiki/Read-copy-update
[3]: https://github.com/brycelelbach/cppnow_presentations_2016/blob/master/01_wednesday/implementing_a_lock_free_atomic_shared_ptr.pdf

### Acknowledgement

Many thanks to `bucienator` for having all the valuable discussions about the implementation, and the API.
