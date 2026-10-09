/* examples/wasm/add.c — WASM demo 模块(验证物料, docs/media-design.md §7)
 *
 * add: i32 签名(契约的 add(2,3)==5 主断言); addf: f64 签名(hn_wasm 的
 * f32/f64 参数/返回起步路径也要有真模块可验)。
 *
 * 编译命令(zig 0.13, 仓库根执行; zig 0.13 的 wasm32-freestanding 不支持
 * --export-all, 只能逐个 --export=<fn>):
 *
 *   zig cc -target wasm32-freestanding --no-standard-libraries \
 *          -Wl,--export=add -Wl,--export=addf -Wl,--no-entry \
 *          examples/wasm/add.c -o examples/wasm/add.wasm
 *
 * 产物 add.wasm 提交入库(魔数 \0asm); 探针 python3 tools/wasm_probe.py
 * 加载它断言 add(2,3)==5 且 addf(2,3)==5。源是纯计算, 无 libc 调用,
 * --no-standard-libraries 下不依赖任何导入。
 */

int add(int a, int b) { return a + b; }

double addf(double a, double b) { return a + b; }
