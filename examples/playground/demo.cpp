// demo.cpp —— 可热改的翻译单元（阶段一 playground）
//
// 玩法：动画跑着的时候，随便改下面的物理逻辑（标了 ✏️ 的地方），
// 然后到另一个终端执行 ./reload.sh —— 不重启进程，动画立刻变。
//
// 规则（阶段一边界）：
//   ✓ 改任何函数的逻辑、改常量、加 static 辅助函数、改字符串
//   ✗ 别增删 g_ 开头的全局变量（那是跨热替换保持的状态，改了会报错回退）
//   ✗ 别引用这个文件之外的符号（printf/数学函数除外）

#include <cmath>
#include <cstdio>

// ---- 世界状态（跨热替换保持；别增删） --------------------------------
double g_x = 1.0;  // 球的横坐标
double g_y = 18.0; // 球的纵坐标（向上为正）
double g_vx = 5.0; // 水平速度
double g_vy = 0.0; // 垂直速度
int g_frame = 0;   // 帧计数（观察状态连续性）

// ---- 物理参数（想改就改，立刻生效） ----------------------------------
const double kGravity = -9.8;     // ✏️ 试试 -1.6（月球）或 -30（木星）
const double kRestitution = 0.85; // ✏️ 落地弹性：1.01 会越弹越高
const double kDt = 0.05;          // 时间步长
// 注意用内嵌数组而不是 const char*：指针是"可变数据"，热替换后仍指向
// 旧字符串（状态保持是按设计生效的）；数组字节随 .rodata 一起换新。
const char kPhysName[] = "v1 地球重力, 弹性0.85"; // ✏️ 改个名字，HUD 会显示

// ✏️ 进阶：加空气阻力 —— 在 step_world 里取消下面这行的注释
// #define AIR_DRAG 1

void step_world() {
#ifdef AIR_DRAG
  g_vx *= 0.995; // 空气阻力：速度衰减
  g_vy *= 0.995;
#endif
  g_vy += kGravity * kDt;
  g_x += g_vx * kDt;
  g_y += g_vy * kDt;

  if (g_y < 0.0) { // 落地反弹
    g_y = 0.0;
    g_vy = -g_vy * kRestitution;
  }
  if (g_x > 76.0)
    g_vx = -fabs(g_vx); // 右墙
  if (g_x < 1.0)
    g_vx = fabs(g_vx); // 左墙
  ++g_frame;
}

void render_world() {
  // 世界坐标 -> 终端行列（行 2..21 画世界，第 23 行画 HUD）
  int row = 21 - static_cast<int>(g_y);
  if (row < 2)
    row = 2;
  if (row > 21)
    row = 21;
  int col = static_cast<int>(g_x);
  if (col < 1)
    col = 1;
  if (col > 76)
    col = 76;

  std::printf("\033[%d;%dHo", row, col + 1);
  std::printf("\033[23;1H[%s] frame=%d  pos=(%.1f, %.1f)  vel=(%.1f, %.1f)\033[K", kPhysName,
              g_frame, g_x, g_y, g_vx, g_vy);
  std::fflush(stdout);
}
