package vref2;

import java.lang.reflect.Method;

/** Golden offsets from the real 1.21.1 client (obfuscated Mth = ayo). */
public final class OffsetRef {
    private OffsetRef() {}

    public static void main(String[] args) throws Exception {
        Class<?> mth = Class.forName("ayo");
        Method getSeed = mth.getDeclaredMethod("b", int.class, int.class, int.class);
        Method clamp = mth.getDeclaredMethod("a", double.class, double.class, double.class);
        getSeed.setAccessible(true);
        clamp.setAccessible(true);
        if (getSeed.getReturnType() != long.class || clamp.getReturnType() != double.class) {
            throw new IllegalStateException("unexpected Mth signatures");
        }

        int[][] positions = {
            {0, 64, 0},
            {1, 10, 2},
            {-1, 70, 3},
            {-8, -64, -8},
            {15, 100, -1},
            {123456, 5, -98765},
            {-30000001, 319, 42},
            {8, 0, 8},
            {49, 151, 14},
            {59, 151, 26},
            {37, 151, 26},
            {25, 151, 18},
            {25, 154, 18},
            {19, 151, 18},
            {1, 151, 26},
            {2, 151, 52},
            {41, 151, 26},
        };
        float[] maxH = {0.25f, 0.125f};
        float[] maxV = {0.2f, 0.1f};

        for (int[] p : positions) {
            long seed = (Long) getSeed.invoke(null, p[0], 0, p[2]);
            System.out.printf("SEED %d %d %d %d%n", p[0], p[1], p[2], seed);
            for (float h : maxH) {
                for (float v : maxV) {
                    double x = horizontal(clamp, seed, 0, h);
                    double y = vertical(seed, v);
                    double z = horizontal(clamp, seed, 8, h);
                    System.out.printf(
                        "OFF %d %d %s %s %.17g %.17g %.17g%n",
                        p[0], p[2], Float.toString(h), Float.toString(v), x, y, z);
                }
            }
        }
    }

    private static double horizontal(Method clamp, long seed, int shift, float maxH) throws Exception {
        long nibble = (seed >> shift) & 0xFL;
        double value = ((double) ((float) nibble / 15.0f) - 0.5) * 0.5;
        double limit = (double) maxH;
        return (Double) clamp.invoke(null, value, -limit, limit);
    }

    private static double vertical(long seed, float maxV) {
        long nibble = (seed >> 4) & 0xFL;
        return ((double) ((float) nibble / 15.0f) - 1.0) * (double) maxV;
    }
}
