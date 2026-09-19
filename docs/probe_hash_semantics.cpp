#include <cstdio>
#include "libipc/memory/resource.h"

int main() {
    ipc::string a{""}, b{""};
    ipc::string c{"loan_broadcast"}, d{"loan_broadcast"};
    auto H = [](ipc::string const &s) { return ipc::hash<ipc::string>{}(s); };

    std::printf("空串:      a==b? %d   hash(a)=%zu hash(b)=%zu  hash相等? %d   c_str a=%p b=%p\n",
                (int)(a == b), H(a), H(b), (int)(H(a) == H(b)),
                (void*)a.c_str(), (void*)b.c_str());
    std::printf("长串:  c==d? %d   hash(c)=%zu hash(d)=%zu  hash相等? %d\n",
                (int)(c == d), H(c), H(d), (int)(H(c) == H(d)));

    ipc::unordered_map<ipc::string, int> m;
    m[a] = 1;
    m[b] = 2;          /* b 与 a 内容相同 */
    std::printf("两个内容相同的空串插入后 map.size()=%zu  (期望 1)\n", m.size());

    ipc::unordered_map<ipc::string, int> m2;
    m2[c] = 1;
    m2[d] = 2;
    std::printf("两个内容相同的长串插入后 map.size()=%zu  (期望 1)\n", m2.size());
    return 0;
}
