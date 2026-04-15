#ifndef STATIC_H
#define STATIC_H

// ลงทะเบียน static fallback — เรียกครั้งเดียวใน main()
// จาก นั้น GET /style.css → serve จาก public/style.css อัตโนมัติ
void static_init(void);

#endif // STATIC_H
