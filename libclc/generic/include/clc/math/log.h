#undef log

// log(x) = log2(x) * (1/log2(e))
#define log(val) (log2(val) * 0.693147181f)
