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
        FLAG,       // 仅作为布尔标志，不接受值
        OPT_BOOL,   // 布尔开关, 值**可选**: 裸写即为真, 也可显式跟 true/false
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
        bool is_opt_bool = false;   // OPT_BOOL: 出现即为真, 但允许显式跟 true/false
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
        a.is_opt_bool = (type == Type::OPT_BOOL);
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
                    /* FLAG 保持"出现即真"不动 —— dzipc_pub 的 --once 就是它, 行为不许变。 */
                    a.value = "true";
                }
                else if (!inline_val.empty())
                {
                    /* `--name=value` 形式对**所有**非 FLAG 类型都优先: 显式写了值就以它为准。
                     * OPT_BOOL 靠这一支拿到 `--watch_handshake=false`。 */
                    a.value = inline_val;
                }
                else if (a.is_opt_bool)
                {
                    /* OPT_BOOL 且没写 `=值`: 裸写 ⇒ 真; `-w false` ⇒ 假。
                     * 只看**紧邻的下一个 token 是不是布尔字面量**决定要不要消费它:
                     *   - `-w -f 4` ⇒ 下一个是 `-f`, 非字面量 ⇒ 判真且**不消费**它。
                     *     (BOOL 的毛病正是无条件消费 ⇒ 把 `-f` 吞成值, 开关静默关闭, 频率一起丢。)
                     *   - `-w false` ⇒ 消费, 值为假。 */
                    if (i + 1 < argc && is_bool_literal(argv[i + 1]))
                    {
                        a.value = argv[++i];
                    }
                    else
                    {
                        a.value = "true";
                    }
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
                printf("\033[31mMissing required parameter: %s \n\033[0m",
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
        std::cout << "Usage: " << this->program_ << " [options] [positional...]\n";
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
    /* 是否为"是/否"布尔字面量 —— OPT_BOOL 据此决定"要不要吃掉下一个 token 当自己的值"。
     * ⛔ 这份清单与 convert<bool>() 的**真值集**必须对齐: 前 4 个为真、后 4 个为假。
     * 漂移的后果是静默反向 —— 例如把 `yes` 列进来却让 convert<bool> 解成 false, 于是
     * `-w yes` 看起来"明明写了 yes"却关掉了开关, 而屏幕上只表现为"什么都没有"。 */
    static bool is_bool_literal(const std::string& s)
    {
        return s == "true" || s == "1" || s == "yes" || s == "on" || s == "false" || s == "0" || s == "no" ||
               s == "off";
    }

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