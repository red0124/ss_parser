#pragma once

#include "common.hpp"
#include "converter.hpp"
#include "exception.hpp"
#include "extract.hpp"
#include "restrictions.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace ss {

template <typename... Options>
class parser {
    constexpr static auto string_error = setup<Options...>::string_error;
    constexpr static auto throw_on_error = setup<Options...>::throw_on_error;

    using multiline = typename setup<Options...>::multiline;
    using error_type = std::conditional_t<string_error, std::string, bool>;

    constexpr static bool escaped_multiline_enabled =
        multiline::enabled && setup<Options...>::escape::enabled;

    constexpr static bool quoted_multiline_enabled =
        multiline::enabled && setup<Options...>::quote::enabled;

    constexpr static bool ignore_header = setup<Options...>::ignore_header;

    constexpr static bool ignore_empty = setup<Options...>::ignore_empty;

public:
    parser(const std::string& file_name,
           const std::string& delim = ss::default_delimiter)
        : file_name_{file_name}, reader_{file_name_, delim} {
        if (reader_.file_) {
            read_line();
            if constexpr (ignore_header) {
                ignore_next();
            } else {
                raw_header_ = reader_.get_buffer();
            }
        } else {
            handle_error_file_not_open();
            eof_ = true;
        }
    }

    parser(const char* const csv_data_buffer, size_t csv_data_size,
           const std::string& delim = ss::default_delimiter)
        : file_name_{"buffer line"},
          reader_{csv_data_buffer, csv_data_size, delim} {
        if (csv_data_buffer) {
            read_line();
            if constexpr (ignore_header) {
                ignore_next();
            } else {
                raw_header_ = reader_.get_buffer();
            }
        } else {
            handle_error_null_buffer();
            eof_ = true;
        }
    }

    parser(parser&& other) = default;
    parser& operator=(parser&& other) = default;

    parser() = delete;
    parser(const parser& other) = delete;
    parser& operator=(const parser& other) = delete;

    bool valid() const {
        if constexpr (string_error) {
            return error_.empty();
        } else if constexpr (throw_on_error) {
            return true;
        } else {
            return !error_;
        }
    }

    const std::string& error_msg() const {
        assert_string_error_defined<string_error>();
        return error_;
    }

    bool eof() const {
        return eof_;
    }

    bool ignore_next() {
        return reader_.read_next();
    }

    template <typename T, typename... Ts>
    T get_object() {
        return to_object<T>(get_next<Ts...>());
    }

    size_t line() const {
        return reader_.line_number_ > 0 ? reader_.line_number_ - 1
                                        : reader_.line_number_;
    }

    size_t position() const {
        return reader_.chars_read_;
    }

    template <typename T, typename... Ts>
    no_void_validator_tup_t<T, Ts...> get_next() {
        std::optional<std::string> error;

        if (!eof_) {
            if constexpr (throw_on_error) {
                try {
                    reader_.parse();
                } catch (const ss::exception& e) {
                    read_line();
                    decorate_rethrow(e);
                }
            } else {
                reader_.parse();
            }
        }

        reader_.update();
        if (!reader_.converter_.valid()) {
            handle_error_invalid_conversion();
            read_line();
            return {};
        }

        clear_error();

        if (eof_) {
            handle_error_eof_reached();
            return {};
        }

        if constexpr (throw_on_error) {
            try {
                auto value = reader_.converter_.template convert<T, Ts...>();
                read_line();
                return value;
            } catch (const ss::exception& e) {
                read_line();
                decorate_rethrow(e);
            }
        }

        auto value = reader_.converter_.template convert<T, Ts...>();

        if (!reader_.converter_.valid()) {
            handle_error_invalid_conversion();
        }

        read_line();
        return value;
    }

    bool field_exists(const std::string& field) {
        if (header_.empty()) {
            split_header_data();
        }

        return header_index(field).has_value();
    }

    template <typename... Ts>
    void use_fields(const Ts&... fields_args) {
        if constexpr (ignore_header) {
            handle_error_header_ignored();
            return;
        }

        if (header_.empty() && !eof()) {
            split_header_data();
        }

        if (!valid()) {
            return;
        }

        auto fields = std::vector<std::string>{fields_args...};

        if (fields.empty()) {
            handle_error_empty_mapping();
            return;
        }

        std::vector<size_t> column_mappings;

        for (const auto& field : fields) {
            if (std::count(fields.begin(), fields.end(), field) != 1) {
                handle_error_field_used_multiple_times(field);
                return;
            }

            auto index = header_index(field);

            if (!index) {
                handle_error_invalid_field(field);
                return;
            }

            column_mappings.push_back(*index);
        }

        reader_.converter_.set_column_mapping(column_mappings, header_.size());
        reader_.next_line_converter_.set_column_mapping(column_mappings,
                                                        header_.size());

        if (line() == 0) {
            ignore_next();
        }
    }

    ////////////////
    // iterator
    ////////////////

    template <bool get_object, typename T, typename... Ts>
    struct iterable {
        struct iterator {
            using value = std::conditional_t<get_object, T,
                                             no_void_validator_tup_t<T, Ts...>>;

            iterator() : parser_{nullptr}, value_{} {
            }

            iterator(parser<Options...>* parser) : parser_{parser}, value_{} {
            }

            iterator(const iterator& other) = default;
            iterator(iterator&& other) = default;

            value& operator*() {
                return value_;
            }

            value* operator->() {
                return &value_;
            }

            iterator& operator++() {
                if (!parser_ || parser_->eof()) {
                    parser_ = nullptr;
                } else {
                    if constexpr (get_object) {
                        value_ =
                            std::move(parser_->template get_object<T, Ts...>());
                    } else {
                        value_ =
                            std::move(parser_->template get_next<T, Ts...>());
                    }
                }
                return *this;
            }

            iterator& operator++(int) {
                return ++*this;
            }

            friend bool operator==(const iterator& lhs, const iterator& rhs) {
                return (lhs.parser_ == nullptr && rhs.parser_ == nullptr) ||
                       (lhs.parser_ == rhs.parser_ &&
                        &lhs.value_ == &rhs.value_);
            }

            friend bool operator!=(const iterator& lhs, const iterator& rhs) {
                return !(lhs == rhs);
            }

        private:
            parser<Options...>* parser_;
            value value_;
        };

        iterable(parser<Options...>* parser) : parser_{parser} {
        }

        iterator begin() {
            return ++iterator{parser_};
        }

        iterator end() {
            return iterator{};
        }

    private:
        parser<Options...>* parser_;
    };

    template <typename... Ts>
    auto iterate() {
        return iterable<false, Ts...>{this};
    }

    template <typename... Ts>
    auto iterate_object() {
        return iterable<true, Ts...>{this};
    }

    ////////////////
    // composite conversion
    ////////////////
    template <typename... Ts>
    class composite {
    public:
        composite(std::tuple<Ts...>&& values, parser& parser)
            : values_{std::move(values)}, parser_{parser} {
        }

        // tries to convert the same line with a different output type
        // only if the previous conversion was not successful,
        // returns composite containing itself and the new output
        // as optional, additionally, if a parameter is passed, and
        // that parameter can be invoked using the converted value,
        // than it will be invoked in the case of a valid conversion
        template <typename... Us, typename Fun = none>
        composite<Ts..., std::optional<no_void_validator_tup_t<Us...>>> or_else(
            Fun&& fun = none{}) {
            using Value = no_void_validator_tup_t<Us...>;
            std::optional<Value> value;
            try_convert_and_invoke<Value, Us...>(value, fun);
            return composite_with(std::move(value));
        }

        // same as or_else, but saves the result into a 'U' object
        // instead of a tuple
        template <typename U, typename... Us, typename Fun = none>
        composite<Ts..., std::optional<U>> or_object(Fun&& fun = none{}) {
            std::optional<U> value;
            try_convert_and_invoke<U, Us...>(value, fun);
            return composite_with(std::move(value));
        }

        std::tuple<Ts...> values() {
            return values_;
        }

        template <typename Fun>
        auto on_error(Fun&& fun) {
            assert_throw_on_error_not_defined<throw_on_error>();
            if (!parser_.valid()) {
                if constexpr (std::is_invocable_v<Fun>) {
                    fun();
                } else {
                    static_assert(string_error,
                                  "to enable error messages within the "
                                  "on_error method "
                                  "callback string_error needs to be enabled");
                    std::invoke(std::forward<Fun>(fun), parser_.error_msg());
                }
            }
            return *this;
        }

    private:
        template <typename T>
        composite<Ts..., T> composite_with(T&& new_value) {
            auto merged_values =
                std::tuple_cat(std::move(values_),
                               std::tuple<T>{parser_.valid()
                                                 ? std::forward<T>(new_value)
                                                 : std::nullopt});
            return {std::move(merged_values), parser_};
        }

        template <typename U, typename... Us, typename Fun = none>
        void try_convert_and_invoke(std::optional<U>& value, Fun&& fun) {
            if (parser_.valid()) {
                return;
            }

            auto tuple_output = try_same<Us...>();
            if (!parser_.valid()) {
                return;
            }

            if constexpr (!std::is_same_v<U, decltype(tuple_output)>) {
                value = to_object<U>(std::move(tuple_output));
            } else {
                value = std::move(tuple_output);
            }

            parser_.try_invoke(*value, std::forward<Fun>(fun));
        }

        template <typename U, typename... Us>
        no_void_validator_tup_t<U, Us...> try_same() {
            parser_.clear_error();
            auto value =
                parser_.reader_.converter_.template convert<U, Us...>();
            if (!parser_.reader_.converter_.valid()) {
                parser_.handle_error_invalid_conversion();
            }
            return value;
        }

        ////////////////
        // members
        ////////////////

        std::tuple<Ts...> values_;
        parser& parser_;
    };

    // tries to convert a line and returns a composite which is
    // able to try additional conversions in case of failure
    template <typename... Ts, typename Fun = none>
    composite<std::optional<no_void_validator_tup_t<Ts...>>> try_next(
        Fun&& fun = none{}) {
        assert_throw_on_error_not_defined<throw_on_error>();
        using Ret = no_void_validator_tup_t<Ts...>;
        return try_invoke_and_make_composite<
            std::optional<Ret>>(get_next<Ts...>(), std::forward<Fun>(fun));
    }

    // identical to try_next but returns composite with object instead of a
    // tuple
    template <typename T, typename... Ts, typename Fun = none>
    composite<std::optional<T>> try_object(Fun&& fun = none{}) {
        assert_throw_on_error_not_defined<throw_on_error>();
        return try_invoke_and_make_composite<
            std::optional<T>>(get_object<T, Ts...>(), std::forward<Fun>(fun));
    }

private:
    // tries to invoke the given function (see below), if the function
    // returns a value which can be used as a conditional, and it returns
    // false, the function sets an error, and allows the invoke of the
    // next possible conversion as if the validation of the current one
    // failed
    template <typename Arg, typename Fun = none>
    void try_invoke(Arg&& arg, Fun&& fun) {
        constexpr bool is_none = std::is_same_v<std::decay_t<Fun>, none>;
        if constexpr (!is_none) {
            using Ret = decltype(try_invoke_impl(arg, std::forward<Fun>(fun)));
            constexpr bool returns_void = std::is_same_v<Ret, void>;
            if constexpr (!returns_void) {
                if (!try_invoke_impl(arg, std::forward<Fun>(fun))) {
                    handle_error_failed_check();
                }
            } else {
                try_invoke_impl(arg, std::forward<Fun>(fun));
            }
        }
    }

    // tries to invoke the function if not none
    // it first tries to invoke the function without arguments,
    // than with one argument if the function accepts the whole tuple
    // as an argument, and finally tries to invoke it with the tuple
    // laid out as a parameter pack
    template <typename Arg, typename Fun = none>
    auto try_invoke_impl(Arg&& arg, Fun&& fun) {
        constexpr bool is_none = std::is_same_v<std::decay_t<Fun>, none>;
        if constexpr (!is_none) {
            if constexpr (std::is_invocable_v<Fun>) {
                return fun();
            } else if constexpr (std::is_invocable_v<Fun, Arg>) {
                return std::invoke(std::forward<Fun>(fun),
                                   std::forward<Arg>(arg));
            } else {
                return std::apply(std::forward<Fun>(fun),
                                  std::forward<Arg>(arg));
            }
        }
    }

    template <typename T, typename Fun = none>
    composite<T> try_invoke_and_make_composite(T&& value, Fun&& fun) {
        if (valid()) {
            try_invoke(*value, std::forward<Fun>(fun));
        }
        return {valid() ? std::move(value) : std::nullopt, *this};
    }

    ////////////////
    // header
    ////////////////

    void split_header_data() {
        ss::splitter<Options...> splitter;
        std::string raw_header_copy = raw_header_;
        splitter.split(raw_header_copy.data(), reader_.delim_);
        for (const auto& [begin, end] : splitter.split_data_) {
            std::string field{begin, end};
            if (std::find(header_.begin(), header_.end(), field) !=
                header_.end()) {
                handle_error_invalid_header(field);
                header_.clear();
                return;
            }
            header_.push_back(std::move(field));
        }
    }

    std::optional<size_t> header_index(const std::string& field) {
        auto it = std::find(header_.begin(), header_.end(), field);

        if (it == header_.end()) {
            return std::nullopt;
        }

        return std::distance(header_.begin(), it);
    }

    ////////////////
    // error
    ////////////////

    void clear_error() {
        if constexpr (string_error) {
            error_.clear();
        } else {
            error_ = false;
        }
    }

    void handle_error_failed_check() {
        constexpr static auto error_msg = " failed check";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_null_buffer() {
        constexpr static auto error_msg = " received null data buffer";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_file_not_open() {
        constexpr static auto error_msg = " could not be opened";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_eof_reached() {
        constexpr static auto error_msg = " read on end of file";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_invalid_conversion() {
        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_)
                .append(" ")
                .append(std::to_string(reader_.line_number_))
                .append(": ")
                .append(reader_.converter_.error_msg());
        } else if constexpr (!throw_on_error) {
            error_ = true;
        }
    }

    void handle_error_header_ignored() {
        constexpr static auto error_msg =
            ": the header row is ignored within the setup it cannot be used";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_invalid_field(const std::string& field) {
        constexpr static auto error_msg =
            ": header does not contain given field: ";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg).append(field);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg + field};
        } else {
            error_ = true;
        }
    }

    void handle_error_field_used_multiple_times(const std::string& field) {
        constexpr static auto error_msg = ": given field used multiple times: ";

        if constexpr (string_error) {
            error_.clear();
            error_.append(file_name_).append(error_msg).append(field);
        } else if constexpr (throw_on_error) {
            throw ss::exception{file_name_ + error_msg + field};
        } else {
            error_ = true;
        }
    }

    void handle_error_empty_mapping() {
        constexpr static auto error_msg = "received empty mapping";

        if constexpr (string_error) {
            error_.clear();
            error_.append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{error_msg};
        } else {
            error_ = true;
        }
    }

    void handle_error_invalid_header(const std::string& field) {
        constexpr static auto error_msg = "header contains duplicates: ";

        if constexpr (string_error) {
            error_.clear();
            error_.append(error_msg).append(error_msg);
        } else if constexpr (throw_on_error) {
            throw ss::exception{error_msg + field};
        } else {
            error_ = true;
        }
    }

    void decorate_rethrow(const ss::exception& e) const {
        static_assert(throw_on_error,
                      "throw_on_error needs to be enabled to use this method");
        throw ss::exception{std::string{file_name_}
                                .append(" ")
                                .append(std::to_string(line()))
                                .append(": ")
                                .append(e.what())};
    }

    ////////////////
    // line reading
    ////////////////

    void read_line() {
        eof_ = !reader_.read_next();
    }

    struct reader {
        reader(const std::string& file_name_, const std::string& delim)
            : delim_{delim}, file_{std::fopen(file_name_.c_str(), "rb")} {
        }

        reader(const char* const buffer, size_t csv_data_size,
               const std::string& delim)
            : delim_{delim}, csv_data_buffer_{buffer},
              csv_data_size_{csv_data_size} {
        }

        reader(reader&& other)
            : buffer_{other.buffer_},
              next_line_buffer_{other.next_line_buffer_},
              helper_buffer_{other.helper_buffer_},
              converter_{std::move(other.converter_)},
              next_line_converter_{std::move(other.next_line_converter_)},
              buffer_size_{other.buffer_size_},
              next_line_buffer_size_{other.next_line_buffer_size_},
              helper_buffer_size{other.helper_buffer_size},
              delim_{std::move(other.delim_)}, file_{other.file_},
              csv_data_buffer_{other.csv_data_buffer_},
              csv_data_size_{other.csv_data_size_},
              curr_char_{other.curr_char_}, crlf_{other.crlf_},
              line_number_{other.line_number_}, chars_read_{other.chars_read_},
              next_line_size_{other.next_line_size_} {
            other.buffer_ = nullptr;
            other.next_line_buffer_ = nullptr;
            other.helper_buffer_ = nullptr;
            other.file_ = nullptr;
        }

        reader& operator=(reader&& other) {
            if (this != &other) {
                buffer_ = other.buffer_;
                next_line_buffer_ = other.next_line_buffer_;
                helper_buffer_ = other.helper_buffer_;
                converter_ = std::move(other.converter_);
                next_line_converter_ = std::move(other.next_line_converter_);
                buffer_size_ = other.buffer_size_;
                next_line_buffer_size_ = other.next_line_buffer_size_;
                helper_buffer_size = other.helper_buffer_size;
                delim_ = std::move(other.delim_);
                file_ = other.file_;
                csv_data_buffer_ = other.csv_data_buffer_;
                csv_data_size_ = other.csv_data_size_;
                curr_char_ = other.curr_char_;
                crlf_ = other.crlf_;
                line_number_ = other.line_number_;
                chars_read_ = other.chars_read_;
                next_line_size_ = other.next_line_size_;

                other.buffer_ = nullptr;
                other.next_line_buffer_ = nullptr;
                other.helper_buffer_ = nullptr;
                other.file_ = nullptr;
                other.csv_data_buffer_ = nullptr;
            }

            return *this;
        }

        ~reader() {
            std::free(buffer_);
            std::free(next_line_buffer_);
            std::free(helper_buffer_);

            if (file_) {
                std::fclose(file_);
            }
        }

        reader() = delete;
        reader(const reader& other) = delete;
        reader& operator=(const reader& other) = delete;

        // read next line each time in order to set eof_
        bool read_next() {
            next_line_converter_.clear_error();
            size_t size = 0;
            while (size == 0) {
                ++line_number_;
                if (next_line_buffer_size_ > 0) {
                    next_line_buffer_[0] = '\0';
                }

                chars_read_ = curr_char_;
                auto [ssize, eof] =
                    get_line(next_line_buffer_, next_line_buffer_size_, file_,
                             csv_data_buffer_, csv_data_size_, curr_char_);

                if (eof) {
                    return false;
                }

                size = remove_eol(next_line_buffer_, ssize);

                if constexpr (!ignore_empty) {
                    break;
                }
            }

            next_line_size_ = size;
            return true;
        }

        void parse() {
            size_t limit = 0;

            if constexpr (escaped_multiline_enabled) {
                while (escaped_eol(next_line_size_)) {
                    if (multiline_limit_reached(limit)) {
                        return;
                    }

                    if (!append_next_line_to_buffer(next_line_buffer_,
                                                    next_line_size_)) {
                        next_line_converter_.handle_error_unterminated_escape();
                        return;
                    }
                }
            }

            next_line_converter_.split(next_line_buffer_, delim_);

            if constexpr (quoted_multiline_enabled) {
                while (unterminated_quote()) {
                    next_line_size_ -= next_line_converter_.size_shifted();

                    if (multiline_limit_reached(limit)) {
                        return;
                    }

                    if (!append_next_line_to_buffer(next_line_buffer_,
                                                    next_line_size_)) {
                        next_line_converter_.handle_error_unterminated_quote();
                        return;
                    }

                    if constexpr (escaped_multiline_enabled) {
                        while (escaped_eol(next_line_size_)) {
                            if (multiline_limit_reached(limit)) {
                                return;
                            }

                            if (!append_next_line_to_buffer(next_line_buffer_,
                                                            next_line_size_)) {
                                next_line_converter_
                                    .handle_error_unterminated_escape();
                                return;
                            }
                        }
                    }

                    next_line_converter_.resplit(next_line_buffer_,
                                                 next_line_size_, delim_);
                }
            }
        }

        void update() {
            std::swap(buffer_, next_line_buffer_);
            std::swap(buffer_size_, next_line_buffer_size_);
            std::swap(converter_, next_line_converter_);
        }

        bool multiline_limit_reached(size_t& limit) {
            if constexpr (multiline::size > 0) {
                if (limit++ >= multiline::size) {
                    next_line_converter_.handle_error_multiline_limit_reached();
                    return true;
                }
            }
            return false;
        }

        bool escaped_eol(size_t size) {
            const char* curr;
            for (curr = next_line_buffer_ + size - 1;
                 curr >= next_line_buffer_ &&
                 setup<Options...>::escape::match(*curr);
                 --curr) {
            }
            return (next_line_buffer_ - curr + size) % 2 == 0;
        }

        bool unterminated_quote() {
            return next_line_converter_.unterminated_quote();
        }

        void undo_remove_eol(char* buffer, size_t& string_end) {
            if (crlf_) {
                std::copy_n("\r\n", 2, buffer + string_end);
                string_end += 2;
            } else {
                std::copy_n("\n", 1, buffer + string_end);
                string_end += 1;
            }
        }

        size_t remove_eol(char*& buffer, size_t ssize) {
            if (buffer[ssize - 1] != '\n') {
                crlf_ = false;
                return ssize;
            }

            size_t size = ssize - 1;
            if (ssize >= 2 && buffer[ssize - 2] == '\r') {
                crlf_ = true;
                size--;
            } else {
                crlf_ = false;
            }

            buffer[size] = '\0';
            return size;
        }

        void realloc_concat(char*& first, size_t& first_size,
                            size_t& buffer_size, const char* const second,
                            size_t second_size) {
            buffer_size = first_size + second_size + 3;
            auto new_first = static_cast<char*>(
                strict_realloc(static_cast<void*>(first), buffer_size));

            first = new_first;
            std::copy_n(second, second_size + 1, first + first_size);
            first_size += second_size;
        }

        bool append_next_line_to_buffer(char*& buffer, size_t& size) {
            undo_remove_eol(buffer, size);

            chars_read_ = curr_char_;
            auto [next_ssize, eof] =
                get_line(helper_buffer_, helper_buffer_size, file_,
                         csv_data_buffer_, csv_data_size_, curr_char_);

            if (eof) {
                return false;
            }

            ++line_number_;
            size_t next_size = remove_eol(helper_buffer_, next_ssize);
            realloc_concat(buffer, size, next_line_buffer_size_, helper_buffer_,
                           next_size);
            return true;
        }

        std::string get_buffer() {
            return std::string{next_line_buffer_, next_line_buffer_size_};
        }

        ////////////////
        // members
        ////////////////
        char* buffer_{nullptr};
        char* next_line_buffer_{nullptr};
        char* helper_buffer_{nullptr};

        converter<Options...> converter_;
        converter<Options...> next_line_converter_;

        size_t buffer_size_{0};
        size_t next_line_buffer_size_{0};
        size_t helper_buffer_size{0};

        std::string delim_;
        FILE* file_{nullptr};

        const char* csv_data_buffer_{nullptr};
        size_t csv_data_size_{0};
        size_t curr_char_{0};

        bool crlf_{false};
        size_t line_number_{0};
        size_t chars_read_{0};

        size_t next_line_size_{0};
    };

    ////////////////
    // members
    ////////////////

    std::string file_name_;
    error_type error_{};
    reader reader_;
    std::vector<std::string> header_;
    std::string raw_header_;
    bool eof_{false};
};

} /* ss */
