#include "math_generator.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint32_t s_counter = 0;

// ========== 内部工具 ==========

static int32_t rand_range(int32_t min, int32_t max)
{
    if (min >= max) return min;
    return min + (int32_t)(esp_random() % (uint32_t)(max - min + 1));
}

// 生成干扰选项：围绕正确答案的近似值，保证4个选项互不相同且非负
static void fill_distractors_math(quiz_problem_t *p, int32_t correct)
{
    int32_t opts[QUIZ_MAX_OPTIONS];
    opts[0] = correct;
    const int delta_count = 12;
    static const int deltas[12] = {1, -1, 2, -2, 3, -3, 5, -5, 10, -10, 11, 12};
    int base = esp_random() % delta_count;

    for (int i = 1; i < QUIZ_MAX_OPTIONS; i++) {
        opts[i] = correct + 20 + i; // 兜底默认值（必定不重复且非负）
        for (int d = 0; d < delta_count; d++) {
            int32_t cand = correct + deltas[(base + d) % delta_count];
            if (cand < 0) continue;
            int dup = 0;
            for (int k = 0; k < i; k++) {
                if (opts[k] == cand) { dup = 1; break; }
            }
            if (!dup) { opts[i] = cand; break; }
        }
    }

    // Fisher-Yates 洗牌
    for (int i = QUIZ_MAX_OPTIONS - 1; i > 0; i--) {
        int j = rand_range(0, i);
        int32_t t = opts[i]; opts[i] = opts[j]; opts[j] = t;
    }

    p->option_count = QUIZ_MAX_OPTIONS;
    p->correct_index = 0;
    for (int i = 0; i < QUIZ_MAX_OPTIONS; i++) {
        snprintf(p->options[i], QUIZ_OPTION_LEN, "%d", (int)opts[i]);
        if (opts[i] == correct) p->correct_index = i;
    }
}

// 从字符串选项中洗牌（用于非数学学科）
static void shuffle_string_options(quiz_problem_t *p, const char *correct,
                                   const char **pool, size_t pool_size)
{
    // 收集干扰项（排除正确答案与重复项）
    const char *distractors[48];
    size_t dcount = 0;
    for (size_t i = 0; i < pool_size && dcount < 48; i++) {
        if (strcmp(pool[i], correct) == 0) continue;
        int dup = 0;
        for (size_t k = 0; k < dcount; k++)
            if (strcmp(distractors[k], pool[i]) == 0) { dup = 1; break; }
        if (!dup) distractors[dcount++] = pool[i];
    }

    const char *opts[QUIZ_MAX_OPTIONS] = {NULL, NULL, NULL, NULL};
    opts[0] = correct;
    int used[48];
    memset(used, 0, sizeof(used));
    for (int i = 1; i < QUIZ_MAX_OPTIONS && dcount > 0; i++) {
        for (int attempt = 0; attempt < 16; attempt++) {
            int idx = rand_range(0, (int32_t)dcount - 1);
            if (!used[idx]) {
                used[idx] = 1;
                opts[i] = distractors[idx];
                break;
            }
        }
    }
    // 若干扰项不足，用占位补齐
    for (int i = 1; i < QUIZ_MAX_OPTIONS; i++) {
        if (!opts[i]) opts[i] = "以上都不是";
    }

    p->option_count = QUIZ_MAX_OPTIONS;
    // 洗牌
    for (int i = QUIZ_MAX_OPTIONS - 1; i > 0; i--) {
        int j = rand_range(0, i);
        const char *t = opts[i]; opts[i] = opts[j]; opts[j] = t;
    }
    for (int i = 0; i < QUIZ_MAX_OPTIONS; i++) {
        snprintf(p->options[i], QUIZ_OPTION_LEN, "%s", opts[i]);
        if (opts[i] == correct) p->correct_index = i;
    }
}

// ========== 数学（初中代数/几何/数列，程序生成）==========

static int32_t ipow(int32_t a, int32_t e)
{
    int32_t r = 1;
    for (int i = 0; i < e; i++) r *= a;
    return r;
}

static void gen_math(quiz_problem_t *p, difficulty_t diff)
{
    (void)diff; // 题库整体按初中及以上水平出题
    int32_t ans;
    int kind = rand_range(0, 8);

    switch (kind) {
    case 0: { // 一元一次方程 ax + b = c
        int32_t a = rand_range(2, 9);
        int32_t x = rand_range(2, 15);
        int32_t b = rand_range(2, 30);
        ans = x;
        snprintf(p->question, sizeof(p->question),
                 "解方程: %dx + %d = %d，x = ?", (int)a, (int)b, (int)(a * x + b));
        break; }
    case 1: { // 一元一次方程 ax - b = c
        int32_t a = rand_range(2, 8);
        int32_t x = rand_range(3, 15);
        int32_t b = rand_range(2, 20);
        ans = x;
        snprintf(p->question, sizeof(p->question),
                 "解方程: %dx - %d = %d，x = ?", (int)a, (int)b, (int)(a * x - b));
        break; }
    case 2: { // 乘方
        int32_t a = rand_range(2, 12);
        int32_t e = rand_range(2, 3);
        ans = ipow(a, e);
        snprintf(p->question, sizeof(p->question),
                 "%d 的 %d 次方等于多少？", (int)a, (int)e);
        break; }
    case 3: { // 完全平方数的算术平方根
        ans = rand_range(6, 25);
        snprintf(p->question, sizeof(p->question),
                 "哪个正整数的平方等于 %d？", (int)(ans * ans));
        break; }
    case 4: { // 百分数
        static const int percents[8] = {10, 15, 20, 25, 40, 50, 60, 75};
        int pc = percents[esp_random() % 8];
        int32_t base = rand_range(2, 20) * 20; // 保证整除
        ans = base * pc / 100;
        snprintf(p->question, sizeof(p->question),
                 "%d 的 %d%% 等于多少？", (int)base, pc);
        break; }
    case 5: { // 有理数运算（含负数）
        int32_t a = rand_range(2, 15);
        int32_t b = rand_range(a + 2, 30);
        ans = b - a;
        snprintf(p->question, sizeof(p->question),
                 "计算: (-%d) + %d = ?", (int)a, (int)b);
        break; }
    case 6: { // 三角形内角和
        int32_t ang1 = rand_range(25, 85);
        int32_t ang2 = rand_range(20, 150 - ang1);
        ans = 180 - ang1 - ang2;
        snprintf(p->question, sizeof(p->question),
                 "三角形两个内角分别为%d度和%d度，第三个内角是多少度？",
                 (int)ang1, (int)ang2);
        break; }
    case 7: { // 等差数列第 n 项
        int32_t a1 = rand_range(1, 9);
        int32_t d = rand_range(2, 8);
        int32_t n = rand_range(8, 15);
        ans = a1 + (n - 1) * d;
        snprintf(p->question, sizeof(p->question),
                 "等差数列首项为%d，公差为%d，第%d项是多少？",
                 (int)a1, (int)d, (int)n);
        break; }
    default: { // 二元一次方程组
        int32_t x = rand_range(4, 18);
        int32_t y = rand_range(1, x - 1);
        ans = x;
        snprintf(p->question, sizeof(p->question),
                 "x + y = %d，x - y = %d，则 x = ?", (int)(x + y), (int)(x - y));
        break; }
    }
    fill_distractors_math(p, ans);
}

// ========== 语文（古诗文名句/文学常识/成语典故）==========

typedef struct { const char *q; const char *a; const char **pool; size_t pool_size; } qa_t;

static const char* s_chinese_pool[] = {
    "李白","杜甫","白居易","苏轼","王维","李商隐","杜牧","辛弃疾","王昌龄","范仲淹","欧阳修",
    "司马迁","司马光","罗贯中","施耐庵","吴承恩","曹雪芹","鲁迅","莫言","祖逖","勾践",
    "刘备","蔺相如","孔子","孟子","诸葛亮","曹操",
    "一览众山小","柳暗花明又一村","化作春泥更护花","蜡炬成灰泪始干","在乎山水之间也","明月几时有",
    "闻鸡起舞","卧薪尝胆","破釜沉舟","完璧归赵","负荆请罪","三顾茅庐","草船借箭","望梅止渴",
    "望岳","使至塞上","赤壁之战","巨鹿之战","和氏璧","史记","资治通鉴","论语","春秋",
    "诗圣","诗仙","诗魔","诗佛","青莲居士","香山居士","易安居士","六一居士",
    "唐代","宋代","汉代","东晋","清代","元代","春秋战国"
};
#define CHINESE_POOL_N (sizeof(s_chinese_pool)/sizeof(s_chinese_pool[0]))

#define CQ(q, a) {q, a, s_chinese_pool, CHINESE_POOL_N}
static const qa_t s_chinese[] = {
    // 古诗文名句
    CQ("\"会当凌绝顶\"的下一句是？", "一览众山小"),
    CQ("\"山重水复疑无路\"的下一句是？", "柳暗花明又一村"),
    CQ("\"落红不是无情物\"的下一句是？", "化作春泥更护花"),
    CQ("\"春蚕到死丝方尽\"的下一句是？", "蜡炬成灰泪始干"),
    CQ("\"明月几时有，把酒问青天\"的作者是谁？", "苏轼"),
    CQ("\"但使龙城飞将在，不教胡马度阴山\"的作者是谁？", "王昌龄"),
    CQ("\"先天下之忧而忧，后天下之乐而乐\"出自谁的作品？", "范仲淹"),
    CQ("\"醉翁之意不在酒\"的下一句是？", "在乎山水之间也"),
    CQ("\"人固有一死，或重于泰山，或轻于鸿毛\"是谁的名言？", "司马迁"),
    CQ("\"会当凌绝顶，一览众山小\"出自杜甫的哪首诗？", "望岳"),
    // 文学常识
    CQ("《史记》的作者是谁？", "司马迁"),
    CQ("被后人称为\"诗圣\"的是谁？", "杜甫"),
    CQ("被后人称为\"诗仙\"的是谁？", "李白"),
    CQ("号\"青莲居士\"的唐代诗人是谁？", "李白"),
    CQ("《资治通鉴》的主编者是谁？", "司马光"),
    CQ("《三国演义》的作者是谁？", "罗贯中"),
    CQ("《水浒传》的作者是谁？", "施耐庵"),
    CQ("《西游记》的作者是谁？", "吴承恩"),
    CQ("《红楼梦》的作者是谁？", "曹雪芹"),
    CQ("\"词\"在哪一朝代发展到鼎盛？", "宋代"),
    CQ("第一位获得诺贝尔文学奖的中国籍作家是谁？", "莫言"),
    CQ("\"六一居士\"是哪位文人的号？", "欧阳修"),
    // 成语典故
    CQ("\"闻鸡起舞\"讲的是谁勤学苦练的故事？", "祖逖"),
    CQ("\"卧薪尝胆\"与哪位君王的复国故事有关？", "勾践"),
    CQ("\"破釜沉舟\"与哪场战役有关？", "巨鹿之战"),
    CQ("\"三顾茅庐\"讲的是谁拜访诸葛亮的故事？", "刘备"),
    CQ("\"完璧归赵\"与哪件宝物有关？", "和氏璧"),
    CQ("\"负荆请罪\"讲的是廉颇向谁请罪？", "蔺相如"),
    CQ("\"草船借箭\"出自哪场战役？", "赤壁之战"),
};
#undef CQ
#define CHINESE_COUNT (sizeof(s_chinese)/sizeof(s_chinese[0]))

static void gen_chinese(quiz_problem_t *p, difficulty_t diff)
{
    (void)diff;
    const qa_t *item = &s_chinese[esp_random() % CHINESE_COUNT];
    snprintf(p->question, sizeof(p->question), "%s", item->q);
    shuffle_string_options(p, item->a, item->pool, item->pool_size);
}

// ========== 英语（初中词汇 + 语法）==========

typedef struct { const char *en; const char *cn; } vocab_t;

static const vocab_t s_vocab[] = {
    {"environment", "环境"}, {"experience", "经验"}, {"knowledge", "知识"},
    {"education", "教育"}, {"government", "政府"}, {"opportunity", "机会"},
    {"technology", "科技"}, {"tradition", "传统"}, {"invention", "发明"},
    {"difference", "差别"}, {"important", "重要的"}, {"different", "不同的"},
    {"necessary", "必要的"}, {"dangerous", "危险的"}, {"patient", "有耐心的"},
    {"valuable", "有价值的"}, {"achieve", "实现"}, {"improve", "改善"},
    {"protect", "保护"}, {"provide", "提供"}, {"receive", "收到"},
    {"celebrate", "庆祝"}, {"decision", "决定"}, {"purpose", "目的"},
    {"ability", "能力"}, {"courage", "勇气"}, {"honesty", "诚实"},
    {"curious", "好奇的"},
};
#define VOCAB_COUNT (sizeof(s_vocab)/sizeof(s_vocab[0]))

static const char* s_cn_words[] = {
    "环境","经验","知识","教育","政府","机会","科技","传统","发明","差别",
    "重要的","不同的","必要的","危险的","有耐心的","有价值的","实现","改善",
    "保护","提供","收到","庆祝","决定","目的","能力","勇气","诚实","好奇的"
};
#define CN_POOL_COUNT (sizeof(s_cn_words)/sizeof(s_cn_words[0]))

static const char* s_grammar_pool[] = {
    "goes","come","went","gone","going","done","used","will","would","shall",
    "is","are","was","were","has","have","had","that","which","who","whose",
    "in","on","at","by","for","with","than","as","because","although"
};
#define GRAMMAR_POOL_N (sizeof(s_grammar_pool)/sizeof(s_grammar_pool[0]))

#define EQ(q, a) {q, a, s_grammar_pool, GRAMMAR_POOL_N}
static const qa_t s_grammar[] = {
    EQ("He ___ to school by bike every day.", "goes"),
    EQ("They ___ playing soccer when it rained.", "were"),
    EQ("I have already ___ my homework.", "done"),
    EQ("This book is more interesting ___ that one.", "than"),
    EQ("The man ___ is talking to Mary is our teacher.", "who"),
    EQ("She is good ___ playing the piano.", "at"),
    EQ("If it rains tomorrow, we ___ stay at home.", "will"),
    EQ("English is ___ as an official language in many countries.", "used"),
    EQ("Neither Tom nor his parents ___ early risers.", "are"),
    EQ("If I ___ you, I would take that job.", "were"),
    EQ("He said he ___ finish the work in two days.", "would"),
    EQ("I won't leave until he ___ back.", "come"),
};
#undef EQ
#define GRAMMAR_COUNT (sizeof(s_grammar)/sizeof(s_grammar[0]))

static void gen_english(quiz_problem_t *p, difficulty_t diff)
{
    (void)diff;
    int mode = rand_range(0, 2); // 0:中->英 1:英->中 2:语法
    if (mode == 2) {
        const qa_t *item = &s_grammar[esp_random() % GRAMMAR_COUNT];
        snprintf(p->question, sizeof(p->question), "%s", item->q);
        shuffle_string_options(p, item->a, item->pool, item->pool_size);
        return;
    }
    size_t idx = esp_random() % VOCAB_COUNT;
    if (mode == 1) {
        snprintf(p->question, sizeof(p->question), "\"%s\" 的中文意思是？", s_vocab[idx].en);
        shuffle_string_options(p, s_vocab[idx].cn, s_cn_words, CN_POOL_COUNT);
    } else {
        snprintf(p->question, sizeof(p->question), "\"%s\" 用英语怎么说？", s_vocab[idx].cn);
        // 答案选项为英文单词
        p->option_count = QUIZ_MAX_OPTIONS;
        const char *opts[QUIZ_MAX_OPTIONS];
        opts[0] = s_vocab[idx].en;
        int i = 1;
        while (i < QUIZ_MAX_OPTIONS) {
            size_t r = esp_random() % VOCAB_COUNT;
            if (strcmp(s_vocab[r].en, s_vocab[idx].en) != 0) {
                int dup = 0;
                for (int k = 0; k < i; k++)
                    if (strcmp(opts[k], s_vocab[r].en) == 0) { dup = 1; break; }
                if (!dup) opts[i++] = s_vocab[r].en;
            }
        }
        for (int k = QUIZ_MAX_OPTIONS - 1; k > 0; k--) {
            int j = rand_range(0, k);
            const char *t = opts[k]; opts[k] = opts[j]; opts[j] = t;
        }
        for (int k = 0; k < QUIZ_MAX_OPTIONS; k++) {
            snprintf(p->options[k], QUIZ_OPTION_LEN, "%s", opts[k]);
            if (opts[k] == s_vocab[idx].en) p->correct_index = k;
        }
    }
}

// ========== 科学（初中物理/化学/生物）==========

static const char* s_science_answers_pool[] = {
    "NaCl","CO2","分子","原子","铁","氢气","氧气","氮气","酸性","氧化",
    "水","黑色","吸热","内能","伏特","帕斯卡","焦耳","欧姆","220","30",
    "I=U/R","阿基米德","伽利略","细胞","线粒体","叶绿体","DNA","小肠",
    "肾脏","神经元","动脉血","葡萄糖","蛋白质",
    "竖直向上","匀速直线","惯性","杠杆","电磁感应","密度","压强","浮力"
};
#define SCIENCE_POOL_N (sizeof(s_science_answers_pool)/sizeof(s_science_answers_pool[0]))

#define SQ(q, a) {q, a, s_science_answers_pool, SCIENCE_POOL_N}
static const qa_t s_science[] = {
    // 化学
    SQ("食盐（氯化钠）的化学式是什么？", "NaCl"),
    SQ("保持物质化学性质的最小微粒是？", "分子"),
    SQ("化学变化中的最小微粒是？", "原子"),
    SQ("电解水时正极产生的气体是？", "氧气"),
    SQ("电解水时负极产生的气体是？", "氢气"),
    SQ("能使澄清石灰水变浑浊的气体是？", "CO2"),
    SQ("空气中含量最多的气体是？", "氮气"),
    SQ("pH 值小于 7 的溶液呈什么性？", "酸性"),
    SQ("铁丝在氧气中燃烧生成什么颜色的固体？", "黑色"),
    SQ("人体中含量最多的物质是？", "水"),
    SQ("地壳中含量最多的元素是？", "氧"),
    SQ("铁生锈是铁与氧气和水共同发生的什么反应？", "氧化"),
    // 物理
    SQ("电压的单位是什么？", "伏特"),
    SQ("压强的单位是什么？", "帕斯卡"),
    SQ("能量和功的单位是什么？", "焦耳"),
    SQ("电阻的单位是什么？", "欧姆"),
    SQ("我国家庭电路的电压是多少伏？", "220"),
    SQ("光在真空中的速度约为每秒多少万千米？", "30"),
    SQ("欧姆定律的表达式是？", "I=U/R"),
    SQ("发现浮力原理（阿基米德原理）的科学家是谁？", "阿基米德"),
    SQ("物体不受力时保持静止或什么运动状态？", "匀速直线"),
    SQ("物体保持原来运动状态的性质叫什么？", "惯性"),
    SQ("撬棒、天平都属于哪种简单机械？", "杠杆"),
    SQ("发电机利用了什么现象把机械能转化为电能？", "电磁感应"),
    SQ("浮力的方向总是怎样的？", "竖直向上"),
    SQ("单位体积某种物质的质量叫什么？", "密度"),
    SQ("冰融化成水的过程需要吸热还是放热？", "吸热"),
    SQ("摩擦生热是把机械能转化为什么能？", "内能"),
    // 生物
    SQ("生物体结构和功能的基本单位是？", "细胞"),
    SQ("细胞中进行呼吸作用释放能量的结构是？", "线粒体"),
    SQ("植物进行光合作用的场所是？", "叶绿体"),
    SQ("人体主要的遗传物质是什么？", "DNA"),
    SQ("人体消化和吸收营养物质的主要器官是？", "小肠"),
    SQ("形成尿液的器官是？", "肾脏"),
    SQ("神经系统结构和功能的基本单位是？", "神经元"),
    SQ("含氧丰富、颜色鲜红的血是？", "动脉血"),
    SQ("人体最重要的供能物质是？", "葡萄糖"),
    SQ("构成人体细胞的基本物质、用于生长发育修复的是？", "蛋白质"),
};
#undef SQ
#define SCIENCE_COUNT (sizeof(s_science)/sizeof(s_science[0]))

static void gen_science(quiz_problem_t *p, difficulty_t diff)
{
    (void)diff;
    const qa_t *item = &s_science[esp_random() % SCIENCE_COUNT];
    snprintf(p->question, sizeof(p->question), "%s", item->q);
    shuffle_string_options(p, item->a, item->pool, item->pool_size);
}

// ========== 历史（中国古代/近代/现代 + 世界史，含年代）==========

static const char* s_history_answers_pool[] = {
    "221","1840","1842","1860","1894","1898","1911","1915","1919","1921",
    "1931","1937","1945","1949","1789","1914","1939","1917","1492","1868","1215","1787",
    "秦始皇","蔡伦","毕昇","李春","张骞","郑和","明成祖","林则徐","孙中山","李鸿章","康有为",
    "陈独秀","李大钊","田汉","瓦特","哥伦布","张择端","李时珍","宋应星","严复","魏源",
    "南京条约","马关条约","辛丑条约","北京条约","天津条约",
    "汉朝","宋朝","隋朝","明朝","清朝","元朝","唐朝",
    "英国","法国","美国","日本","德国","俄国",
    "洛阳","长安","开封","北京","南京",
    "蒸汽机","电气时代","人文主义","马克思主义","工业革命","第二次鸦片战争","戊戌变法","洋务运动","五四运动","辛亥革命"
};
#define HISTORY_POOL_N (sizeof(s_history_answers_pool)/sizeof(s_history_answers_pool[0]))

#define HQ(q, a) {q, a, s_history_answers_pool, HISTORY_POOL_N}
static const qa_t s_history[] = {
    // 中国古代
    HQ("秦统一六国、建立秦朝是哪一年（公元前）？", "221"),
    HQ("中国历史上第一位皇帝是谁？", "秦始皇"),
    HQ("改进造纸术的东汉发明家是谁？", "蔡伦"),
    HQ("活字印刷术的发明者是谁？", "毕昇"),
    HQ("出使西域、开辟丝绸之路的汉代使者是谁？", "张骞"),
    HQ("隋朝开凿的大运河以哪座城市为中心？", "洛阳"),
    HQ("设计建造赵州桥的工匠是谁？", "李春"),
    HQ("郑和下西洋开始于哪位皇帝在位时期？", "明成祖"),
    HQ("指南针应用于航海始于哪个朝代？", "宋朝"),
    HQ("《清明上河图》的作者是谁？", "张择端"),
    HQ("《本草纲目》的作者是谁？", "李时珍"),
    HQ("《天工开物》的作者是谁？", "宋应星"),
    HQ("科举制度正式创立于哪个朝代？", "隋朝"),
    // 中国近代
    HQ("鸦片战争爆发于哪一年？", "1840"),
    HQ("中国近代第一个不平等条约是？", "南京条约"),
    HQ("领导虎门销烟的民族英雄是谁？", "林则徐"),
    HQ("英法联军火烧圆明园发生在哪次战争中？", "第二次鸦片战争"),
    HQ("甲午中日战争爆发于哪一年？", "1894"),
    HQ("《马关条约》割让台湾给哪个国家？", "日本"),
    HQ("戊戌变法发生在哪一年？", "1898"),
    HQ("李鸿章是哪场运动在地方的代表人物？", "洋务运动"),
    HQ("八国联军侵华后签订的条约是？", "辛丑条约"),
    HQ("领导辛亥革命、推翻帝制的是谁？", "孙中山"),
    HQ("辛亥革命爆发于哪一年？", "1911"),
    HQ("新文化运动开始的标志是哪一年《新青年》创刊？", "1915"),
    HQ("翻译《天演论》、提出\"物竞天择\"的启蒙思想家是谁？", "严复"),
    // 中国现代
    HQ("五四运动爆发于哪一年？", "1919"),
    HQ("中国共产党成立于哪一年？", "1921"),
    HQ("九一八事变爆发于哪一年？", "1931"),
    HQ("七七事变标志着全面抗战开始于哪一年？", "1937"),
    HQ("抗日战争胜利是在哪一年？", "1945"),
    HQ("中华人民共和国成立于哪一年？", "1949"),
    HQ("《义勇军进行曲》的词作者是谁？", "田汉"),
    // 世界史
    HQ("法国大革命爆发于哪一年？", "1789"),
    HQ("第一次世界大战爆发于哪一年？", "1914"),
    HQ("第二次世界大战全面爆发于哪一年？", "1939"),
    HQ("俄国十月革命发生在哪一年？", "1917"),
    HQ("1492年横渡大西洋到达美洲的航海家是谁？", "哥伦布"),
    HQ("日本明治维新开始于哪一年？", "1868"),
    HQ("英国限制王权的《大宪章》签署于哪一年？", "1215"),
    HQ("世界上第一部成文宪法是哪国1787年宪法？", "美国"),
    HQ("第一次工业革命的标志性发明是什么？", "蒸汽机"),
    HQ("改良蒸汽机、推动工业革命的关键人物是谁？", "瓦特"),
    HQ("第二次工业革命把人类带入了什么时代？", "电气时代"),
    HQ("文艺复兴的核心思想是什么？", "人文主义"),
    HQ("《共产党宣言》标志着什么思想的诞生？", "马克思主义"),
    HQ("第一次工业革命最早开始于哪个国家？", "英国"),
};
#undef HQ
#define HISTORY_COUNT (sizeof(s_history)/sizeof(s_history[0]))

static void gen_history(quiz_problem_t *p, difficulty_t diff)
{
    (void)diff;
    const qa_t *item = &s_history[esp_random() % HISTORY_COUNT];
    snprintf(p->question, sizeof(p->question), "%s", item->q);
    shuffle_string_options(p, item->a, item->pool, item->pool_size);
}

// ========== 公共接口 ==========

const char* quiz_subject_name(subject_t subject)
{
    static const char* names[] = {"数学", "语文", "英语", "科学", "历史"};
    if (subject >= SUBJECT_MAX) return "未知";
    return names[subject];
}

void quiz_generator_init(void)
{
    s_counter = 0;
}

int quiz_generate(quiz_problem_t *problem, difficulty_t difficulty)
{
    // 学科分布：数学10% 英语10% 语文20% 科学30% 历史30%（偏重史地/物理通识）
    static const subject_t s_dist[] = {
        SUBJECT_MATH, SUBJECT_ENGLISH,
        SUBJECT_CHINESE, SUBJECT_CHINESE,
        SUBJECT_SCIENCE, SUBJECT_SCIENCE, SUBJECT_SCIENCE,
        SUBJECT_HISTORY, SUBJECT_HISTORY, SUBJECT_HISTORY,
    };
    subject_t subj = s_dist[esp_random() % (sizeof(s_dist) / sizeof(s_dist[0]))];
    return quiz_generate_subject(problem, subj, difficulty);
}

int quiz_generate_subject(quiz_problem_t *problem, subject_t subject, difficulty_t difficulty)
{
    if (!problem || subject >= SUBJECT_MAX || difficulty >= DIFFICULTY_MAX) return -1;

    memset(problem, 0, sizeof(quiz_problem_t));
    problem->subject = subject;
    problem->difficulty = difficulty;
    problem->id = ++s_counter;
    problem->from_ai = false;

    switch (subject) {
        case SUBJECT_MATH:    gen_math(problem, difficulty);    break;
        case SUBJECT_CHINESE: gen_chinese(problem, difficulty); break;
        case SUBJECT_ENGLISH: gen_english(problem, difficulty); break;
        case SUBJECT_SCIENCE: gen_science(problem, difficulty); break;
        case SUBJECT_HISTORY: gen_history(problem, difficulty); break;
        default: return -1;
    }
    return 0;
}
