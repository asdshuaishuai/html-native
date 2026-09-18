/* EventToken.h — WebView2.h 需要的最小定义(C 兼容)
 *
 * WebView2.h 尾部 include "EventToken.h" 用 EventToken 作为事件订阅
 * 句柄。新 Windows SDK 已把它改名 EventRegistrationToken 并移入 winrt
 * 头(C++ only), WebView2.h 仍按旧名包含 —— 这里提供等价的最小定义。
 * 本文件放在 WebView2.h 同目录: 引号 include 先命中源文件目录, 因此
 * 编译时不会去系统 SDK 找那份 C++ 版本。
 */
#ifndef __eventtoken_min_h__
#define __eventtoken_min_h__

typedef struct EventToken {
    long long value;
} EventToken;

/* 新版 SDK 的等价名(同一 POD, WebView2.h 两处名字都引用) */
typedef struct EventRegistrationToken {
    long long value;
} EventRegistrationToken;

#endif /* __eventtoken_min_h__ */
