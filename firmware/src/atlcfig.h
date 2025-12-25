// 提供键盘交互能力
extern int Keyhit_impl();

#define Keyhit Keyhit_impl
#define Keybreak() { int ch = Keyhit(); broken = (ch == 27) || (ch == 'q') || (ch == 'Q'); }
