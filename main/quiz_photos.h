// quiz_photos.h — flash 里的出题图库：挂载、随机换图、按名显示
#ifndef QUIZ_PHOTOS_H
#define QUIZ_PHOTOS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 挂载 /spiffs 并扫描 /spiffs/photos。@return 图库张数，<=0 表示失败 */
int quiz_photos_init(void);

/** 显示一张。name 为 NULL/""/"rand" 时随机，否则按文件名（.jpg 后缀可省）。
 *  @return 0=成功；info 回填 "文件名 | 解码/刷屏耗时" 之类的诊断 */
int quiz_photos_show(const char *name, char *info, size_t sz);

/** 起周期换图任务（默认 120 秒一张，先立刻出一张） */
void quiz_photos_start_task(void);

int  quiz_photos_count(void);
int  quiz_photos_interval(void);
int  quiz_photos_set_interval(int sec);      // 限 2..3600，返回生效值
const char *quiz_photos_last(void);
const char *quiz_photos_name(int idx);       // 越界返回 ""

#ifdef __cplusplus
}
#endif

#endif // QUIZ_PHOTOS_H
