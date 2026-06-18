#pragma once

#include <exec/any_sender_of.hpp>
#include <exception>
#include <stdexec/execution.hpp>

namespace asiortc {

template <class T> struct any_sender_trait {
    using type =
        exec::any_sender<exec::any_receiver<stdexec::completion_signatures<
            stdexec::set_value_t(T), stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>>>;
};

template <> struct any_sender_trait<void> {
    using type =
        exec::any_sender<exec::any_receiver<stdexec::completion_signatures<
            stdexec::set_value_t(), stdexec::set_error_t(std::exception_ptr),
            stdexec::set_stopped_t()>>>;
};

template <class T> using any_sender = typename any_sender_trait<T>::type;

} // namespace asiortc
