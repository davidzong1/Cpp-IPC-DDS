#ifndef ARG_PARSER_HPP
#define ARG_PARSER_HPP

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

class ArgParser
{
public:
    enum class Type {
        STRING,
        INT,
        DOUBLE,
        BOOL,
        FLAG,   // 仅作为布尔标志，不接受值
    };

    struct Argument
    {
        std::string long_name;       // --input
        std::string short_name;      // -i
        std::string help;            // Help information
        std::string default_value;   // Default value (as string)
        std::string value;           // Actual parsed value
        Type type = Type::STRING;
        bool required = false;
        bool is_flag = false;   // Boolean flag, e.g., --verbose
        bool is_positional = false;
        bool present = false;   // Whether it was explicitly set
    };

    ArgParser(std::string prog = "program", std::string desc = "")
        : program_(std::move(prog))
        , description_(std::move(desc))
    {
        add_argument("--help", "-h", "Display help information and exit", Type::BOOL);
    }

    // 添加一个参数；以 - 开头视为可选参数，否则视为位置参数
    ArgParser& add_argument(const std::string& name, const std::string& short_name = "", const std::string& help = "",
                            Type type = Type::STRING, bool required = false, const std::string& default_value = "")
    {
        Argument a;
        a.long_name = name;
        a.short_name = short_name;
        a.help = help;
        a.type = type;
        a.required = required;
        a.default_value = default_value;
        a.value = default_value;
        a.is_flag = (type == Type::FLAG);
        a.is_positional = name.empty() || name[0] != '-';
        a.present = !default_value.empty();

        size_t idx = args_.size();
        args_.push_back(std::move(a));
        if (!name.empty())
            index_[name] = idx;
        if (!short_name.empty())
            index_[short_name] = idx;
        return *this;
    }

    // 解析命令行
    void parse(int argc, char* argv[])
    {
        std::vector<std::string> positionals;

        for (int i = 1; i < argc; ++i)
        {
            std::string tok = argv[i];

            if (tok == "-h" || tok == "--help")
            {
                print_help();
                std::exit(0);
            }

            if (tok.size() > 1 && tok[0] == '-')
            {
                std::string key = tok;
                std::string inline_val;
                // 支持 --key=value 形式
                auto eq = tok.find('=');
                if (eq != std::string::npos)
                {
                    key = tok.substr(0, eq);
                    inline_val = tok.substr(eq + 1);
                }

                auto it = index_.find(key);
                if (it == index_.end())
                {
                    printf("\033[31mUnknown parameter: %s\033[0m\n", key.c_str());
                    print_help();
                    std::exit(1);
                }

                Argument& a = args_[it->second];
                if (a.is_flag)
                {
                    a.value = "true";
                }
                else if (!inline_val.empty())
                {
                    a.value = inline_val;
                }
                else
                {
                    if (i + 1 >= argc)
                    {
                        printf("\033[31mParameter %s is missing a value\033[0m\n", key.c_str());
                        print_help();
                        std::exit(1);
                    }
                    a.value = argv[++i];
                }
                a.present = true;
            }
            else
            {
                positionals.push_back(tok);
            }
        }

        // 把剩余 token 依次分配给位置参数
        size_t p = 0;
        for (auto& a : args_)
        {
            if (a.is_positional && p < positionals.size())
            {
                a.value = positionals[p++];
                a.present = true;
            }
        }

        // 必填检查
        for (auto& a : args_)
        {
            if (a.required && !a.present)
            {
                printf("\033[31mMissing required parameter: %s\033[0m",
                       (a.long_name.empty() ? a.short_name.c_str() : a.long_name.c_str()));
                print_help();
                std::exit(1);
            }
        }
    }

    // 模板取值，自动转换类型
    template<typename T>
    T get(const std::string& name) const
    {
        auto it = index_.find(name);
        if (it == index_.end())
        {
            printf("\033[31mParameter not found: %s\033[0m\n", name.c_str());
            print_help();
            std::exit(1);
        }
        return convert<T>(args_[it->second].value);
    }

    bool has(const std::string& name) const
    {
        auto it = index_.find(name);
        return it != index_.end() && args_[it->second].present;
    }

    void print_help() const
    {
        std::cout << "Usage: " << program_ << " [options] [positional...]\n";
        if (!description_.empty())
            std::cout << "\n" << description_ << "\n";
        std::cout << "\nOptions:\n";
        for (const auto& a : args_)
        {
            std::ostringstream key;
            if (!a.short_name.empty())
                key << a.short_name << ", ";
            key << a.long_name;
            std::cout << "  " << std::left << std::setw(22) << key.str() << a.help;
            if (!a.default_value.empty())
                std::cout << " [Default: " << a.default_value << "]";
            if (a.required)
                std::cout << " (Required)";
            std::cout << "\n";
        }
    }

private:
    template<typename T>
    static T convert(const std::string& s)
    {
        std::stringstream ss(s);
        T v{};
        ss >> v;
        return v;
    }

    std::string program_;
    std::string description_;
    std::vector<Argument> args_;
    std::unordered_map<std::string, size_t> index_;
};

// 特化：string 不要走 stringstream（会被空格截断）
template<>
inline std::string ArgParser::convert<std::string>(const std::string& s)
{
    return s;
}

// 特化：bool 接受多种写法
template<>
inline bool ArgParser::convert<bool>(const std::string& s)
{
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

#endif   // ARG_PARSER_HPP