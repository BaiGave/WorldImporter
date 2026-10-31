package vref3;

import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.util.Locale;

/**
 * Golden vectors for WorldImporter/tests/biome_blend_tests.cpp.
 * Calls the real 1.21.1 (obfuscated) BiomeManager and LinearCongruentialGenerator.
 * Package vref3 avoids the signed jar's sealed net.minecraft packages.
 *
 * Classpath = 1.21.1.jar plus every jar under worlds/1.21.1/libraries:
 *   javac -cp "$CP" vref3/VRef.java vref3/Noise.java
 *   java -cp ".:$CP" vref3.VRef
 * Names: ddy BiomeManager, ayj LinearCongruentialGenerator, jd BlockPos, ddy$a NoiseBiomeSource.
 */
public final class VRef {
    private VRef() {}

    public static void main(String[] args) throws Exception {
        Class<?> lcg = Class.forName("ayj"); // LinearCongruentialGenerator
        Method next = lcg.getMethod("a", long.class, long.class); // next(long, long)

        Class<?> bm = Class.forName("ddy"); // BiomeManager
        Method obfuscate = bm.getMethod("a", long.class); // obfuscateSeed
        Method fiddled = bm.getDeclaredMethod(
                "a", long.class, int.class, int.class, int.class, double.class, double.class, double.class);
        fiddled.setAccessible(true); // getFiddledDistance
        Class<?> source = Class.forName("ddy$a"); // NoiseBiomeSource
        Class<?> pos = Class.forName("jd"); // BlockPos
        Constructor<?> bmCtor = bm.getConstructor(source, long.class);
        Constructor<?> posCtor = pos.getConstructor(int.class, int.class, int.class);
        Method getBiome = bm.getMethod("a", pos); // getBiome(BlockPos)

        long[] seeds = {0L, 1L, -1L, 42L, 123456789L, -4530634556500121041L, 2345L, Long.MIN_VALUE, 987654321012345678L};
        for (long seed : seeds) {
            long hashed = (Long) obfuscate.invoke(null, seed);
            System.out.println("SEED " + seed + " " + hashed);
        }

        long[][] pairs = {
            {0L, 0L},
            {1L, 1L},
            {-1L, 5L},
            {12345L, -99L},
            {6364136223846793005L, 1442695040888963407L},
            {Long.MIN_VALUE, 7L},
            {0x123456789ABCDEFL, -1L}
        };
        for (long[] pair : pairs) {
            long out = (Long) next.invoke(null, pair[0], pair[1]);
            System.out.println("LCG " + pair[0] + " " + pair[1] + " " + out);
        }

        long zoom = (Long) obfuscate.invoke(null, 123456789L);
        System.out.println("ZOOM " + zoom);
        int[][] quarts = {{0, 0, 0}, {1, 2, 3}, {-1, -2, -3}, {15, -4, 8}, {4, 16, -5}};
        double[] fracs = {0.0, 0.25, 0.5, 0.75};
        for (int[] q : quarts) {
            for (double fx : fracs) {
                double dist = (Double) fiddled.invoke(null, zoom, q[0], q[1], q[2], fx, 0.25, 0.5);
                System.out.printf(Locale.US, "FID %d %d %d %.2f %s%n",
                        q[0], q[1], q[2], fx, Long.toUnsignedString(Double.doubleToRawLongBits(dist)));
            }
        }

        final int[] chosen = new int[3];
        InvocationHandler handler = (proxy, method, margs) -> {
            if ("getNoiseBiome".equals(method.getName()) && margs != null && margs.length == 3) {
                chosen[0] = (Integer) margs[0];
                chosen[1] = (Integer) margs[1];
                chosen[2] = (Integer) margs[2];
            }
            return null;
        };
        Object src = Proxy.newProxyInstance(source.getClassLoader(), new Class<?>[] {source}, handler);
        Object manager = bmCtor.newInstance(src, zoom);
        int[][] blocks = {
            {0, 64, 0}, {1, 64, 0}, {2, 63, -1}, {3, 64, 3}, {4, 64, 4},
            {15, 70, 15}, {16, 64, 16}, {-1, 64, -1}, {-5, 30, 7},
            {8, 8, 8}, {100, -64, 100}, {-16, 319, -16}, {7, 63, 8}, {-8, -33, 9}
        };
        for (int[] b : blocks) {
            Object blockPos = posCtor.newInstance(b[0], b[1], b[2]);
            getBiome.invoke(manager, blockPos);
            System.out.printf(Locale.US, "QUART %d %d %d %d %d %d%n",
                    b[0], b[1], b[2], chosen[0], chosen[1], chosen[2]);
        }

        long worldZoom = (Long) obfuscate.invoke(null, -4530634556500121041L);
        Object manager2 = bmCtor.newInstance(src, worldZoom);
        System.out.println("WORLDZOOM " + worldZoom);
        int[][] blocks2 = {{0, 80, 0}, {17, 63, -20}, {-2, 100, 5}, {31, 64, 31}};
        for (int[] b : blocks2) {
            Object blockPos = posCtor.newInstance(b[0], b[1], b[2]);
            getBiome.invoke(manager2, blockPos);
            System.out.printf(Locale.US, "WQUART %d %d %d %d %d %d%n",
                    b[0], b[1], b[2], chosen[0], chosen[1], chosen[2]);
        }
    }
}
