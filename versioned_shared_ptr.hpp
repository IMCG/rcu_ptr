#include <memory>
#include <atomic>

template <typename T>
class versioned_shared_ptr {

    std::shared_ptr<T> sp;

public:
    // template <typename Y>
    // versioned_shared_ptr(const std::shared_ptr<Y>& r) {}

    versioned_shared_ptr& operator=(const versioned_shared_ptr& rhs) {
        write(rhs.sp);
    }

    std::shared_ptr<const T> read() { return std::atomic_load(&sp); }

    void write(const std::shared_ptr<T>& r) {
        auto sp_l = std::atomic_load(&sp);
        auto exchange_result = false;
        //while (sp_l && !exchange_result) {
        while (!exchange_result) {
            // True if exchange was performed
            // If sp == sp_l (share ownership of the same pointer),
            //   assigns r into sp
            // If sp != sp_l, assigns sp into sp_l
            exchange_result =
                std::atomic_compare_exchange_strong(&sp, &sp_l, r);
        }
    }
};