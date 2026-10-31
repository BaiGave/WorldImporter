package vref3;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.List;

/**
 * Swamp grass noise vectors. Builds PerlinSimplexNoise directly
 * (eoj + WorldgenRandom dzx + LegacyRandomSource dyz(2345) + octave {0}).
 * Biome's own static init cannot run outside a bootstrapped registry.
 * getValue(x * 0.0225, z * 0.0225, false) < -0.1 selects 5011004, otherwise 6975545.
 */
public class Noise {
    public static void main(String[] args) throws Exception {
        Class<?> rnd = Class.forName("ayw");
        Class<?> legacy = Class.forName("dyz");
        Class<?> worldgen = Class.forName("dzx");
        Class<?> simplexWrap = Class.forName("eoj");
        Object inner = legacy.getConstructor(long.class).newInstance(2345L);
        Object wrapped = worldgen.getConstructor(rnd).newInstance(inner);
        Object noise = simplexWrap.getConstructor(rnd, List.class).newInstance(wrapped, List.of(Integer.valueOf(0)));
        Method get = simplexWrap.getMethod("a", double.class, double.class, boolean.class);
        int[] xs = {0, 1, 8, -3, 15, 100, -40, 7, 64, -16};
        int[] zs = {0, 1, -2, 9, 40, -7, 3, 80, 15, -16};
        for (int i = 0; i < xs.length; i++) {
            double x = xs[i], z = zs[i];
            double v = (Double) get.invoke(noise, x * 0.0225, z * 0.0225, Boolean.FALSE);
            int color = v < -0.1 ? 5011004 : 6975545;
            System.out.println("SWAMP " + xs[i] + " " + zs[i] + " " + color + " " + Long.toUnsignedString(Double.doubleToRawLongBits(v)));
        }
    }
}
