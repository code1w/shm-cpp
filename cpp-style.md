# 代码规范

## 一、背景

项目开发是个团队合作的过程，提高合作效率尤其重要。如果每个参与者都遵从自己的编码风格，久而久之，这份代码的阅读门槛会越来越高，解决编码问题的难度也会水涨船高。

遵循统一的编码规范可以提高代码的阅读效率，这有助于我们相互理解和维护彼此的代码，以及更快地排查和修复代码中的错误。

编码规范通常也包括对代码质量的要求，例如对命名规范、注释规范、错误处理规范等的规定。遵循这些规范可以帮助我们编写更加健壮、高效和安全的代码，变相提高代码质量。

长此以往，也可以帮助我们养成良好的编码习惯，从而提高个人的编码水平，让我们写出更加高效、可读和易于维护的代码。

> **本规范具有强制性。** 所有规则均以"必须（MUST）/ 禁止（MUST NOT）/ 应当（SHOULD）/ 不应（SHOULD NOT）"加以区分，Code Review 阶段将对违反强制性规则的代码提出拒绝意见。

---

## 二、代码风格基准

C++ 代码风格以 [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) 为基础，并结合团队现状做了若干定制修改。两者冲突时，以本文档为准。

---

## 三、格式规范

### 3.1 缩进与空格

- **必须**使用 **4 个空格**缩进；**禁止**使用 TAB。
- **禁止**行末尾出现多余的空白字符（trailing whitespace）。
- 每行代码**不超过 120 个字符**（ASCII），超出时须手动换行并对齐。
- 换行时，续行的缩进比起始行多 4 个空格；若是函数参数换行，参数对齐到左括号后一列。

```cpp
// 参数换行：对齐到左括号后
bool result = SomeLongFunctionName(argument_one,
                                   argument_two,
                                   argument_three);

// 或者全部参数独占一行，缩进 4 格（两种均可，同文件保持一致）
bool result = SomeLongFunctionName(
    argument_one,
    argument_two,
    argument_three);
```

### 3.2 大括号与 Allman 风格

- **必须**采用 Allman 风格：所有 `{` 均**另起一行**，与前序语句独占一行，**禁止** K&R / 埃及风格。
- `else` / `else if` / `catch` 必须在 `}` 的**下一行**开始。

```cpp
// 正确
void Foo(int value)
{
    if (value > 0)
    {
        DoSomething();
    }
    else
    {
        DoOther();
    }
}

// 错误：{ 不换行
void Foo(int value) {
    if (value > 0) {
```

- **单行 `if` / `for` / `while`**：**必须**加大括号，即使只有一条语句。

```cpp
// 正确
if (!ok)
{
    return;
}

// 错误：省略大括号
if (!ok)
    return;
```

### 3.3 空格规则

- **所有双目运算符**（`=`, `+`, `-`, `*`, `/`, `%`, `<`, `>`, `<=`, `>=`, `==`, `!=`, `&&`, `||`, `&`, `|`, `^`, `<<`, `>>`）前后**必须**各加一个空格。
- **单目运算符**（`!`, `~`, `-`（取负）, `++`, `--`）与操作数之间**不加**空格。
- **函数参数默认值**的 `=` **不加**空格。
- **关键字**（`if`, `for`, `while`, `switch`, `return`）后的 `(` 前**必须**加一个空格；**函数名**后的 `(` 前**禁止**加空格。
- 逗号 `,` 后**必须**加一个空格，前**禁止**加空格。
- 三目运算符 `?:` 前后各加一个空格。
- 有运算符优先级歧义时（混合 `&&` 与 `||`、位运算与算术运算等），**必须**加括号明确优先级。

```cpp
int x = 100;
bool b = (x > 0) && (x < 200);     // 必须加括号
int y = x * 2 + 1;
bool flag = !b;                     // 单目不加空格
int func(int a, int b=0);           // 默认值不加空格
if (flag)                           // if 后加空格
    Call(flag);                     // 函数名后不加空格
x = (flag) ? 1 : 0;                // 三目前后加空格
```

### 3.4 指针与引用

- 指针 `*` 和引用 `&` **必须**紧靠**类型**，与变量名之间加一个空格。
- 函数返回类型中的 `*` / `&` 紧靠返回类型。
- **禁止**在同一行用 `*` 同时声明多个变量（如 `int* a, b`）。

```cpp
int* p = nullptr;       // * 紧靠类型
int& r = value;         // & 紧靠类型
const char* GetName();  // 返回类型中 * 紧靠类型
```

### 3.5 空行

- 函数定义之间**必须**空一行。
- 类内逻辑分块（成员变量、public/protected/private）之间空一行。
- 函数体内部，不同逻辑块之间可空一行，但连续空行**不超过一行**。
- 文件末尾**必须**以一个换行符结束（POSIX 规定）。

---

## 四、命名规范

### 4.1 总则

- 名称必须**自注释**（self-documenting）：不使用 `tmp`、`data2`、`foo`、`x` 等无意义名称（循环变量 `i`, `j` 的极短范围内除外）。
- **禁止**使用拼音命名；**禁止**使用匈牙利前缀（如 `iCount`, `szName`）。
- 缩写仅在**业界通用**缩写（`fd`、`buf`、`len`、`pos`、`idx`、`err`、`msg`、`cfg`）时允许使用；自造缩写**必须**在首次出现的注释中说明。

### 4.2 文件与目录

| 类型 | 规则 | 示例 |
|------|------|------|
| 源文件 | 全小写，`_` 分隔，`.cc` 扩展名 | `session_manager.cc` |
| 头文件 | 全小写，`_` 分隔，`.h` 扩展名 | `http_client.h` |
| 目录 | 全小写，`_` 分隔 | `core/` |

> 本项目约定：库头文件使用 `.h`，可执行源文件使用 `.cc`。

### 4.3 命名空间

- 全小写，`_` 分隔单词。
- **必须**在命名空间结束的 `}` 后加注释 `// namespace <name>`。
- **禁止**在头文件中使用 `using namespace xxx`（`.cc` 文件中允许有限使用）。

```cpp
namespace myapp
{

// ...

}  // namespace myapp
```

### 4.4 类与结构体

- **必须**使用 PascalCase（大驼峰）。
- 纯数据聚合（无不变量、无非平凡成员函数）用 `struct`；其余用 `class`。

```cpp
class HttpClient { /* … */ };
struct RequestHeader { /* … */ };
```

### 4.5 函数与方法

- **必须**使用 PascalCase，包括全局函数、静态函数和成员函数。
- 动词开头，表达**行为**：`TryConnect`、`CreateSocket`、`SendMessage`。
- 返回 `bool` 的函数以 `Is`/`Has`/`Can`/`Check` 开头。
- 获取属性的 accessor 以 `Get`/`Set` 开头（PascalCase 规则适用：`GetId`, `SetTimeout`）。

```cpp
bool IsValid() const;
int  GetId() const;
void SetTimeout(int ms);
bool TryConnect(const std::string& host, uint16_t port);
```

### 4.6 变量

| 类型 | 规则 | 示例 |
|------|------|------|
| 局部变量 | 全小写，`_` 分隔 | `retry_count`, `buf_size` |
| 函数参数 | 全小写，`_` 分隔 | `host_addr`, `timeout_ms` |
| 类成员变量 | 全小写，`_` 分隔，末尾 `_` | `socket_fd_`, `is_connected_` |
| struct 成员变量 | 全小写，`_` 分隔，无后缀 | `msg_type`, `payload_len` |
| 全局变量 | `g` + PascalCase | `gEventLoop`, `gConfig` |
| 静态局部变量 | 与局部变量相同 | `retry_count` |

```cpp
bool gIsRunning = false;          // 全局变量

class TcpClient
{
    int       socket_fd_ = -1;    // 类成员：末尾 _
    bool      is_connected_ = false;
};

struct PacketHeader
{
    uint16_t msg_type;            // struct 成员：无后缀
    uint32_t payload_len;
};

void Send(const void* data, std::size_t len)  // 参数：无后缀
{
    std::size_t bytes_sent = 0;               // 局部变量
}
```

### 4.7 常量与枚举

- 编译期常量（`constexpr`、`const` 全局/静态）：`k` + PascalCase。
- 枚举值：`k` + PascalCase（强类型 `enum class` **必须**使用）。
- **禁止**使用 `#define` 定义数值常量；**必须**用 `constexpr` 或 `enum class` 替代。

```cpp
constexpr std::size_t kMaxClients    = 16;
constexpr std::size_t kRingCapacity  = 8 * 1024 * 1024;

enum class ConnState
{
    kIdle,
    kHandshaking,
    kConnected,
    kClosed,
};
```

### 4.8 宏

- 宏名全大写，`_` 分隔：`APP_ASSERT`, `LIKELY`。
- 能用 `constexpr`、`inline` 函数或模板替代的，**禁止**使用宏。
- 头文件保护宏格式：`<PROJECT>_<PATH>_<FILE>_HPP_`，如 `MYAPP_NET_HTTP_CLIENT_HPP_`。

---

## 五、头文件规范

### 5.1 Include Guard

所有头文件**必须**使用宏 Include Guard，**禁止**使用 `#pragma once`（可移植性更差）：

```cpp
#ifndef MYAPP_NET_HTTP_CLIENT_HPP_
#define MYAPP_NET_HTTP_CLIENT_HPP_

// ...

#endif  // MYAPP_NET_HTTP_CLIENT_HPP_
```

### 5.2 Include 顺序

每个分组内按字母序排列；分组之间以空行分隔：

1. 配对头文件（若 `.cc` 文件有对应 `.h`，先 include 该头文件）
2. C 系统头文件（`<unistd.h>`, `<sys/mman.h>` 等）
3. C++ 标准库（`<atomic>`, `<vector>` 等）
4. 第三方库头文件
5. 本项目头文件

```cpp
// http_client.cc
#include "net/http_client.h"       // 1. 配对头文件

#include <sys/socket.h>              // 2. C 系统头文件
#include <unistd.h>

#include <cstdint>                   // 3. C++ 标准库
#include <string>
#include <vector>

#include "net/tcp_socket.h"        // 5. 本项目头文件
#include "utils/logger.h"
```

### 5.3 前置声明

- 仅在能**明确**减少 include 依赖时使用前置声明。
- 对标准库类型、模板类（如 `std::vector`）**禁止**使用前置声明。
- 头文件中**禁止**包含不必要的 include（"不需要就不 include"）。

### 5.4 内联函数

- 函数体超过 **10 行**的，**不应**定义为 `inline`（编译器可忽略，但代码表意要准确）。
- 头文件中定义的函数体（非 `inline` 关键字，但隐式 inline）同此限制。

---

## 六、类规范

### 6.1 访问控制顺序

类成员**必须**按如下顺序排列：

```
public:    // 类型别名、嵌套类、静态常量
public:    // 构造/析构
public:    // 公有方法
protected: // 保护方法
private:   // 私有方法
private:   // 成员变量
```

### 6.2 构造函数

- 不应在构造函数中进行复杂初始化逻辑或可能失败的操作；此类初始化应提供显式的 `Init()` 方法并返回错误码/bool。
- 单参数构造函数**必须**标记为 `explicit`，防止隐式转换。
- 构造函数**尽量**使用成员初始化列表（initializer list），而非在函数体内赋值。

```cpp
class FileHandle
{
 public:
    explicit FileHandle(int fd) noexcept : fd_(fd) {}
    // ...
};
```

### 6.3 拷贝与移动

- 明确表达拷贝/移动语义意图：若类不可拷贝，**必须**显式 `= delete`；可移动则显式定义或 `= default`。
- 遵循**零法则（Rule of Zero）**：若不需要自定义析构，则拷贝/移动也不用自定义；需要自定义析构则**必须**同时考虑拷贝/移动（Rule of Five）。

```cpp
class Buffer
{
 public:
    Buffer(const Buffer&)            = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
};
```

### 6.4 虚函数

- 基类析构函数如果有虚函数，**必须**声明为 `virtual`（或 `= default`）。
- 派生类覆盖虚函数**必须**同时加 `override`，**禁止**单独加 `virtual`。
- **禁止**在构造/析构函数中调用虚函数。

```cpp
class Base
{
 public:
    virtual ~Base() = default;
    virtual void Process() = 0;
};

class Derived : public Base
{
 public:
    void Process() override;    // 只用 override，不重复写 virtual
};
```

### 6.5 `[[nodiscard]]`

- 返回错误码、资源句柄、或调用者**必须**检查的值的函数，**必须**标记 `[[nodiscard]]`。

```cpp
[[nodiscard]] bool TryConnect(const std::string& host, uint16_t port);
[[nodiscard]] int  GetId() const noexcept;
```

---

## 七、函数规范

### 7.1 函数长度

- 函数体**不应**超过 **60 行**（不含注释和空行）。超过时应考虑拆分逻辑子函数。
- 函数的**圈复杂度**（cyclomatic complexity）**不应**超过 **10**，即嵌套/分支层数不应过深。

### 7.2 参数规范

- 参数数量**不应**超过 **6 个**；超过时使用结构体或 Builder 模式传参。
- 输入参数优先用 `const T&`（避免拷贝）；基本类型（`int`, `bool`, `std::size_t` 等）直接值传递。
- 输出参数优先用返回值或 `std::tuple`/`std::pair`，避免用指针出参（RAII 资源除外）。
- 函数参数**一行代码只允许定义一个变量**；返回值和参数语义须在注释中说明。

### 7.3 `const` 正确性

- 不修改对象状态的成员函数**必须**标记 `const`。
- 不修改的指针参数/返回指针**必须**加 `const`。
- **禁止** `const_cast` 去除常量性（极少数与 C API 交互时须添加注释说明理由）。

### 7.4 `noexcept`

- 确定不抛异常的函数（析构函数、移动构造/赋值、简单 getter、交换函数）**必须**标记 `noexcept`。
- 性能敏感的公共 API（底层库、热路径接口）**应当**以返回错误码代替异常，并标记 `noexcept`。

### 7.5 早返回（Early Return）

- **鼓励**使用早返回减少嵌套层数：先处理错误/边界条件并 `return`，正常逻辑放在函数末尾。

```cpp
bool ProcessRequest(const void* data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return false;
    if (size > kMaxPayloadSize)
        return false;
    // 主逻辑...
    return true;
}
```

### 7.6 单一职责

- 每个函数只做**一件事**，函数名必须精确描述该事情。不应把"构建"和"发送"、"校验"和"存储"混在同一个函数里。

---

## 八、变量规范

### 8.1 变量声明

- **必须**在声明的同时赋初值；**禁止**使用未初始化的变量。
- **一行代码只允许定义一个变量**，**禁止** `int a, b = 0;`。
- 变量声明应**尽量靠近其首次使用处**，不应在函数顶部集中声明所有变量（C 风格）。

```cpp
// 正确
uint32_t val1 = 0;
uint32_t val2 = 0;

// 错误
uint32_t val1, val2;   // 未初始化，且一行定义两个
```

### 8.2 作用域

- **优先**使用最小作用域：能用局部变量的就不用全局/文件作用域变量。
- 全局可变状态（`g` 前缀变量）**必须**有明确的线程安全说明（或声明为单线程访问）。
- **禁止**在头文件中定义非 `inline`/`constexpr` 的全局变量（ODR 违规）。

### 8.3 类型选择

- **必须**使用定长整数类型（`uint8_t`, `int32_t`, `uint64_t` 等，`<cstdint>`）表示协议字段、内存偏移、大小限制等关键数据；**禁止**在此类场景使用 `int`/`long`（平台相关宽度）。
- 内存大小和偏移量**必须**使用 `std::size_t` 或 `uint64_t`；**禁止**用 `int` 表示。
- **禁止**隐式窄化转换（如 `uint64_t` 赋给 `int`）；需要时须显式 `static_cast` 并添加注释。

---

## 九、注释规范

遵循 Doxygen 规则，优先使用行注释 `///`。

### 9.1 通用原则

- 注释解释**为什么（Why）**，而非**是什么（What）**——代码本身表达"做什么"，注释表达背后的意图、约束、权衡。
- 注释**必须**与代码保持同步；过时的注释比没有注释更有害，Code Review 必须同时审查注释。
- 注释符号（`//`, `///`, `/**`）与正文间**必须**空一格。
- **禁止**注释掉代码后长期搁置（超过一个版本）；应删除并依赖版本控制历史。

### 9.2 文件注释

每个头文件/源文件**必须**在首行（include guard 之前或之后）添加文件注释：

```cpp
/**
 * @file http_client.h
 * @brief 轻量级 HTTP/1.1 客户端（C++17）
 *
 * 详细描述（可选）：用途、线程安全、性能特征等。
 */
```

### 9.3 类 / 结构体注释

类和结构体**必须**有 `@brief` 注释，复杂类还应说明线程安全性、所有权、生命周期限制：

```cpp
/**
 * @brief 线程安全的对象池
 *
 * 线程安全：所有公有方法可由多线程并发调用。
 * 生命周期：Pool 销毁前，所有借出对象必须已归还。
 */
template <typename T>
class ObjectPool { /* … */ };
```

### 9.4 函数注释

公有函数**必须**在声明处（头文件）写 Doxygen 注释；私有且非平凡的函数**应当**注释：

```cpp
/**
 * @brief 发送 HTTP GET 请求，超时或网络错误时返回错误码
 * @param url        请求 URL，不含协议前缀
 * @param timeout_ms 超时阈值（毫秒），0 表示不限时
 * @return           响应体字符串；发生错误时返回 std::nullopt
 * @note             线程安全，可由多线程并发调用
 */
[[nodiscard]] std::optional<std::string> Get(
    const std::string& url, int timeout_ms = 5000) noexcept;
```

### 9.5 成员变量注释

- 行内后置注释使用 `///<`：
- 非显而易见的成员变量**必须**注释其含义和单位（如 `///< 超时阈值，单位毫秒`）。

```cpp
class TcpClient
{
    int     socket_fd_   = -1;     ///< 底层套接字描述符，-1 表示未连接
    int     timeout_ms_  = 5000;   ///< 读写超时阈值，单位毫秒
    bool    is_blocking_ = true;   ///< true = 阻塞模式，false = 非阻塞模式
};
```

### 9.6 实现注释与 TODO

```cpp
// 先尝试读缓存，未命中再发起网络请求（减少延迟）
auto cached = cache_.Get(key);
if (!cached)
    cached = FetchFromRemote(key);

// TODO(alice): 当前线性扫描；条目超过 1000 时改为哈希索引
// FIXME(bob): 时钟回拨时 timestamp 比较逻辑失效
```

- `TODO` 格式：`// TODO(负责人): 说明`
- `FIXME` 格式：`// FIXME(负责人): 说明`
- **禁止**无名无说明的 `// TODO` 堆积。

---

## 十、`#include` 与依赖管理

- **禁止**循环依赖；架构分层（底层 ← 上层），下层**禁止** include 上层头文件。
- 每个模块只 include 自己**直接**依赖的头文件，**禁止**依赖间接 include（"include what you use"）。
- **禁止**在头文件中 `using namespace std;` 或其他命名空间展开。

---

## 十一、现代 C++（C++17）规范

### 11.1 类型推导

- **鼓励**在类型冗长且右侧已明确时使用 `auto`：

```cpp
auto conn = ConnectionPool::Acquire();   // 右侧已明确类型
auto it   = index_map_.find(key);        // 迭代器类型冗长
int  count = GetCount();                 // 基本类型不用 auto，保留显式类型
```

- **禁止**在函数签名（参数、返回值）中使用 `auto` 代替具体类型（降低可读性）。

### 11.2 范围 for 与算法

- 遍历容器**优先**使用范围 for，只有需要下标时才用下标循环。
- 适合 STL 算法（`std::for_each`, `std::transform`, `std::find_if`）时**应当**优先使用算法代替手写循环。

```cpp
for (auto& client : clients_)
{
    client.Reset();
}
```

### 11.3 结构化绑定

- `std::pair`/`std::tuple`/结构体多返回值时**应当**使用结构化绑定：

```cpp
auto [status, body] = SendRequest(url, payload);
auto [it, inserted] = cache_.emplace(key, value);
```

### 11.4 智能指针

- 堆对象所有权**必须**用 `std::unique_ptr`；共享所有权用 `std::shared_ptr`。
- **禁止**裸 `new`/`delete`（RAII 资源包装内部的实现除外）。
- 函数传递非所有权的对象用原始指针或引用，**禁止**传 `shared_ptr` 来"借用"（性能损耗）。

### 11.5 `std::optional` / `std::variant`

- 可能不存在的返回值**应当**用 `std::optional<T>` 代替哨兵值（如 `-1`, `nullptr`），使语义明确。
- 多态数据**应当**优先考虑 `std::variant` 代替虚函数（零堆分配，缓存友好）。

### 11.6 `constexpr`

- 编译期可计算的常量和函数**必须**标记 `constexpr`；**禁止**用 `#define` 定义数值常量。

```cpp
static constexpr std::size_t kHeaderSize = sizeof(PacketHeader);
static constexpr std::size_t kMaxPacket  = kHeaderSize + kMaxPayload;
static_assert(kMaxPacket <= 65536, "Packet exceeds UDP MTU");
```

### 11.7 `static_assert`

- 模板参数约束、大小/对齐假设**必须**用 `static_assert` 在编译期验证：

```cpp
static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
static_assert(sizeof(RingHeader) == 128, "RingHeader size mismatch");
```

---

## 十二、错误处理规范

### 12.1 基本原则

- 本项目（Linux 底层 IPC）以**返回值**传递错误，**禁止**抛出异常（系统调用语境）。
- 系统调用失败**必须**检查返回值并处理；**禁止**忽略 `[[nodiscard]]` 的返回值。
- 错误路径**必须**记录足够信息（`errno`、调用上下文）；**禁止**静默丢弃错误。

### 12.2 断言

- 内部不变量（合同式前置条件）使用 `assert()`，编译 Release 时自动去除。
- 运行时必须成立的条件（配置错误、资源不足）应返回错误而非 `assert`。
- **禁止**在 `assert()` 中有副作用表达式。

```cpp
assert(shm != nullptr);             // 内部不变量
assert((cap & (cap - 1)) == 0);    // 必须为 2 的幂
```

### 12.3 RAII

- 所有资源（fd、mmap、锁）**必须**通过 RAII 封装管理，**禁止**裸 `close()`/`munmap()` 散落在业务逻辑中。

---

## 十三、并发与原子操作规范

### 13.1 内存顺序

- **禁止**在没有充分理由时使用 `memory_order_relaxed`；无锁数据结构中必须显式标注内存顺序，并在注释中说明选择理由。
- 数据发布使用 `memory_order_release`；数据消费使用 `memory_order_acquire`，保证 acquire-release 配对。

```cpp
// 发布新版本：确保数据写入对其他线程可见后再更新版本号
version_.store(new_ver, std::memory_order_release);

// 读取版本：与 release 配对，保证读到完整数据
uint32_t ver = version_.load(std::memory_order_acquire);
```

### 13.2 缓存行填充

- 被不同线程频繁访问的原子变量**必须**各自独占一个缓存行（64 字节），防止 false sharing：

```cpp
struct alignas(64) ProducerConsumerCounters
{
    std::atomic<uint64_t> produced;
    char pad1[64 - sizeof(std::atomic<uint64_t>)];  ///< 填充至 64 字节
    std::atomic<uint64_t> consumed;
    char pad2[64 - sizeof(std::atomic<uint64_t>)];
};
```

### 13.3 线程安全文档

- 每个类**必须**在注释中说明线程安全级别：
  - `@thread-safety none`：不支持并发访问
  - `@thread-safety SPSC`：单生产者单消费者
  - `@thread-safety thread-safe`：完全线程安全

---

## 十四、性能敏感代码规范

### 14.1 热路径原则

- 热路径函数（每消息调用一次及以上）**必须**避免：堆分配（`new`/`malloc`）、系统调用（除非必要）、锁争用、虚函数调用。
- **禁止**在热路径上使用 `std::string` 进行临时拼接；使用栈上缓冲区或 `std::string_view`。

### 14.2 内存对齐

- 跨进程共享的 POD 结构**必须**显式指定 `alignas` 和大小，并用 `static_assert` 验证，防止不同编译单元 ABI 不一致。

### 14.3 分支预测

- 错误分支（异常情况）**应当**使用 `[[unlikely]]`；正常分支可用 `[[likely]]`（C++20；C++17 可改用 `__builtin_expect`）：

```cpp
if (size > kMaxWriteSize) [[unlikely]]
{
    return 0;
}
```

---

## 十五、安全规范

### 15.1 缓冲区与边界

- 所有内存写入操作**必须**在写入前校验边界，**禁止**依赖调用方保证大小合法。
- **禁止**使用 `strcpy`、`sprintf`、`gets` 等不安全 C 函数；使用 `strncpy`/`snprintf`/`std::string` 替代。
- 来自共享内存、网络、文件的数据视为**不可信输入**，在使用前**必须**校验（长度、范围、枚举值合法性）。

### 15.2 整数安全

- **禁止**有符号/无符号整数混用进行比较或运算（UB / 隐式截断）。
- 整数运算前**应当**检查溢出，尤其是用于计算偏移、索引、大小的场合。

### 15.3 资源句柄

- 文件描述符、套接字、数据库连接等句柄**必须**通过 RAII 封装（如 `FileHandle`、`SocketGuard`）持有，**禁止**裸句柄在业务逻辑中流转。
- 句柄传递跨越所有权边界时，**必须**明确转移语义（移动语义或显式 `Release()`/`Acquire()`），**禁止**隐式共享裸句柄导致双重释放。
- **禁止**将平台相关句柄（如 fd、HANDLE）直接序列化或存入持久化存储；跨进程传递须使用操作系统提供的专用机制。

---

## 十六、代码审查检查清单

提交代码前，作者和 Reviewer 均应逐项确认：

| # | 检查项 |
|---|--------|
| 1 | 变量、函数、类命名符合规范，无拼音/无意义命名 |
| 2 | 所有变量声明时已初始化 |
| 3 | 大括号 Allman 风格，`if`/`for`/`while` 均有大括号 |
| 4 | 公有函数有完整 Doxygen 注释（`@brief`/`@param`/`@return`） |
| 5 | `[[nodiscard]]` 返回值未被忽略 |
| 6 | 系统调用错误已检查并处理 |
| 7 | 无未初始化变量、无内存泄漏（RAII） |
| 8 | 热路径无堆分配、无不必要系统调用 |
| 9 | 原子操作内存顺序正确，已有注释说明 |
| 10 | 不可信输入已边界校验 |
| 11 | 无 `#define` 数值常量，使用 `constexpr`/`enum class` |
| 12 | 无 `using namespace` 污染头文件 |
| 13 | 文件末尾有且仅有一个换行符 |
| 14 | 注释与代码同步，无过时注释 |
