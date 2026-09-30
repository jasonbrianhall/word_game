/* The few libm functions wordle.cpp's sound synthesis and layout use.
 * Computed in double precision and rounded to float, which is plenty for
 * audio and pixel positions. No libm headers: they'd clash with the builtins. */

static const double PI_D = 3.14159265358979323846;

/* sin(x) for |x| <= pi/2 (Taylor series to x^15: error < 1e-12). */
static double sin_core(double x) {
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n <= 7; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

static double sin_d(double x) {
    /* Reduce to [-pi, pi], then fold into [-pi/2, pi/2]. */
    double k = x / (2 * PI_D);
    long long ki = (long long)(k < 0 ? k - 0.5 : k + 0.5);
    x -= (double)ki * 2 * PI_D;
    if (x > PI_D / 2) x = PI_D - x;
    else if (x < -PI_D / 2) x = -PI_D - x;
    return sin_core(x);
}

static double exp_d(double x) {
    if (x > 700) x = 700;
    if (x < -700) return 0.0;
    /* x = k*ln2 + r, |r| <= ln2/2 */
    const double LN2 = 0.69314718055994530942;
    long long k = (long long)(x / LN2 + (x < 0 ? -0.5 : 0.5));
    double r = x - (double)k * LN2, term = 1.0, sum = 1.0;
    for (int n = 1; n <= 14; n++) { term *= r / n; sum += term; }
    union { double d; unsigned long long u; } p;
    p.u = (unsigned long long)(k + 1023) << 52;       /* 2^k */
    return sum * p.d;
}

float sinf(float x) { return (float)sin_d(x); }
float cosf(float x) { return (float)sin_d((double)x + PI_D / 2); }
double sin(double x) { return sin_d(x); }
double cos(double x) { return sin_d(x + PI_D / 2); }
float expf(float x) { return (float)exp_d(x); }
double exp(double x) { return exp_d(x); }
float tanhf(float x) {
    if (x > 10.f) return 1.f;
    if (x < -10.f) return -1.f;
    double e = exp_d(2.0 * x);
    return (float)((e - 1.0) / (e + 1.0));
}
long lround(double x) { return (long)(x < 0 ? x - 0.5 : x + 0.5); }
long lroundf(float x) { return lround(x); }
