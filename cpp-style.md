# 代码规范

## 一、背景

项目开发是个团队合作的过程，提高合作效率尤其重要。如果每个参与者都遵从自己的编码风格，久而久之，这份代码的阅读门槛会越来越高，解决编码问题的难度也会水涨船高。

遵循统一的编码规范可以提高代码的阅读效率，这有助于我们相互理解和维护彼此的代码，以及更快地排查和修复代码中的错误。

编码规范通常也包括对代码质量的要求，例如对命名规范、注释规范、错误处理规范等的规定。遵循这些规范可以帮助我们编写更加健壮、高效和安全的代码，变相提高代码质量。

长此以往，也可以帮助我们养成良好的编码习惯，从而提高个人的编码水平，让我们写出更加高效、可读和易于维护的代码。

## 二、代码风格

C++ 代码风格请参考 [Google C++ Style](https://google.github.io/styleguide/cppguide.html)

| 表格 | 说明 |
| ---- | ---- |

> 各种风格本身无好坏，为了提高同事间阅读代码的效率，降低合作门槛，才选了比较流行的 Google C++ 代码风格进行编码，请大家务必遵守规范。

## 三、C++ 代码规范

### 3.1 基础风格

代码的基本风格基于 Google C++ Style，为了更能够适应团队的状况做一定的修改后的版本。

- 代码中的缩进都为 **4 个空格**，禁止使用 TAB 的缩进。
- 所有的 `{` 必须与前序逻辑不在同一行中，需要另起一行。

```cpp
void Foo(int value)
{
    if (value)
    {
        // ...
    }
    else
    {
        // ...
    }
}
```

- 所有的双目运算符，必须在前后都加上空格；单目运算符和函数参数默认值的等号可不加空格；单行语句有运算符优先级的加括号区分。

```cpp
int x = 100;
bool b = (x > 100);             // 需加括号
bool b2 = !b;                   // 单目运算符不加空格
bool b3 = (x <= 100) ? x : 0;
int func(int a, int b=0);       // 参数默认值不加空格
```

- `if`、`while`、`for` 等后面紧接的 `(` 需用空格与前序代码分隔。

```cpp
if (...) { }
while (...) {}
for (int x = 0; x < 100; ++x) {}
```

- 指针 `*` 和引用 `&` 与变量之间增加空格；强制换行时，`&` 与返回类型保持同一行。

```cpp
int value = 100;
int* p = &value;
int& r = value;
```

- 无法通过构造函数初始化的变量必须赋初值，尤其成员变量。

```cpp
class Player
{
private:
    void* ptr = nullptr; // 必须初始化成员变量
};
```

```cpp
ReturnType functionName(ArgumentType argument=ArgumentType())
{
    ReturnType return_value;
    ArgumentType &reference_variable = argument;
    ArgumentType *point_variable = &argument;
    uint32_t integer_variable_example = 0; // 必须初始化变量
    bool boolean_varibale_example = false;
    boolean_varibale_example = (integer_variable_example > 0);
    if (!boolean_varibale_example)
    {
        integer_variable_example = integer_variable_example + integer_variable_example;
    }
    return return_value;
}
```

- 一行代码只允许定义一个变量。

```cpp
/**
 * uint32_t val1, val2;
 * 错误1: 一行代码只允许一个变量定义
 * 错误2: 变量没有初始化
 */
uint32_t val1 = 0;
uint32_t val2 = 0;
```

- 鼓励使用多出口，尽量在最短路径上 `return`。

```cpp
bool CheckAttack(Agent& a, Agent& b)
{
    if (!CheckDistance(a, b))
        return false; // 最短路径return掉
    if (!CheckEnemy(a, b))
        return false;
    return true;
}
```

### 3.2 命名规范

#### 3.2.1 文件、目录命名

- 文件名 / 目录名：全小写字母，`_` 分隔单词
- 扩展名：源文件 `.cc`、头文件 `.h`、内联文件 `.inc`

```plaintext
scenes_server/scene_user.h
scenes_server/scene_user.cc
shared/script_global_func.inc
```

#### 3.2.2 名字空间

全小写字母，`_` 分隔单词

```cpp
namespace base { /* … */ }
namespace aa_bb_cc { /* … */ }
```

#### 3.2.3 类、结构

Pascal 命名法（首字母大写，无分隔符）

```cpp
class SceneUser { /* … */ };
struct BirthplaceData { /* … */ };
```

#### 3.2.4 函数

Pascal 命名法，全局 / 成员函数统一遵循

```cpp
void SendCmdToMe(const void* cmd_data, DWORD cmd_len);
```

#### 3.2.5 变量

全小写字母，`_` 分隔，按场景区分：

- **类成员变量**：末尾加 `_`
- **结构成员变量**：无后缀
- **全局变量**：`g` + Pascal 命名

```cpp
bool gIsRunning;              /// 全局变量: g+Pascal命名规则

class Foo
{
    bool is_running_;         /// 类成员变量: 全小写+_+后缀_
};

struct Foo2
{
    bool is_running;          /// 结构成员变量: 全小写+_
};

void SendCmdToMe(const void* cmd_data, DWORD cmd_len) /// 参数: 全小写+_
{
    int total_len;            /// 局部变量: 全小写+_
}
```

#### 3.2.6 常量

`k` + Pascal 命名法

```cpp
const int kFlagValue = 10000;

void Test()
{
    static const std::string kName = "TEST";
}
```

### 3.3 头文件

- **`#define` 保护**：所有头文件使用 `#define` 防止多重包含，格式：`<PATH>_<FILE>_H_`
- **前置声明**：优先使用前置声明，减少 `#include` 依赖
- **内联函数**：仅 10 行及以内函数定义为内联函数
- **`#include` 顺序**：相关头文件 → C 库 → C++ 库 → 其他项目 → 本项目

```cpp
#include "scenes_server/scene_user.h"

#include <sys/types.h>
#include <unistd.h>

#include <hash_map>
#include <vector>

#include "shared/logging.h"
#include "shared/lua.h"
#include "scenes_server/attr_manager.h"
#include "scenes_server/buff_manager.h"
```

### 3.4 注释

遵循 Doxygen 规则，优先使用行注释 `//`

#### 3.4.1 通用规则

- **单行注释**：`/// 注释内容`
- **后置注释**：`变量 ///< 注释内容`
- 注释符号与文字间空一格
- **段注释**：`/** 注释 */`

```cpp
/// 计数变量
int count = 0;

int count = 0; ///< 计数变量

// 正确写法
/**
 * 段注释示例
 */
```

#### 3.4.2 文件注释

```cpp
/**
 * @file player.h
 * @brief Player逻辑处理
 * @date 2020-12-15
 */
```

#### 3.4.3 类 / 结构注释

```cpp
/** @brief 玩家实例定义 */
class Player {};
```

#### 3.4.4 函数注释

```cpp
/**
 * @brief 函数说明
 * @param msg 消息数据结构
 * @param size 消息大小
 * @return 返回值
 * @see 参考Init
 */
bool ClientMsg(Protocol Msg, int size);
```

#### 3.4.5 成员分组注释

```cpp
/** @name RPC Req and Rsp
 * @param req_msg：请求消息
 * @param rsp_msg：返回消息
 * @return 0：成功
 */
///@{
/** @brief 增加邮件 */
int32_t add_mail(const ::ss_proto::AddMailReq &req_msg, std::shared_ptr<::ss_proto::AddMailAck> &rsp_msg) override;
///@}
```

#### 3.4.6 变量 / 实现 / TODO 注释

```cpp
// 成员变量注释
private:
    /// 表项总数，-1表示未初始化
    int num_total_entries_;

// 实现注释
// Divide result by two...
for (int i = 0; i < result->size(); i++) {}

// TODO注释
// @todo(jiqimao):需要添加负载均衡
// todo（dayu）：待优化
```
