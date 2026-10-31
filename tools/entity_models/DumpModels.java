import java.io.*;
import java.lang.reflect.*;
import java.nio.file.*;
import java.util.*;

/**
 * Dumps every vanilla entity LayerDefinition (LayerDefinitions.createRoots) as baked ModelPart trees.
 * usage: java -cp client.jar:libs/* :. DumpModels out.json [client_mappings.txt]
 * With a Mojang mappings file the obfuscated jar (<=1.21.x) is resolved by name.
 */
public class DumpModels {
    static Map<String, String> clsMap = new HashMap<>();            // named -> obf
    static Map<String, Map<String, String>> memMap = new HashMap<>(); // named class -> (named member -> obf)

    static void loadMappings(String path) throws IOException {
        String cur = null;
        for (String line : Files.readAllLines(Paths.get(path))) {
            if (line.startsWith("#") || line.isBlank()) continue;
            if (!line.startsWith(" ")) {
                String[] p = line.split(" -> ");
                cur = p[0];
                clsMap.put(cur, p[1].substring(0, p[1].length() - 1));
                memMap.put(cur, new HashMap<>());
            } else {
                String[] p = line.trim().split(" -> ");
                String left = p[0];
                String name;
                int paren = left.indexOf('(');
                if (paren >= 0) {
                    String s = left.substring(0, paren);
                    name = s.substring(s.lastIndexOf(' ') + 1) + "()";
                } else name = left.substring(left.lastIndexOf(' ') + 1);
                memMap.get(cur).putIfAbsent(name, p[1]);
            }
        }
    }
    static Class<?> cls(String named) throws Exception { return Class.forName(clsMap.getOrDefault(named, named)); }
    static String mem(String cls, String name) {
        Map<String, String> m = memMap.get(cls);
        String plain = name.endsWith("()") ? name.substring(0, name.length() - 2) : name;
        return m == null ? plain : m.getOrDefault(name, plain);
    }
    static Object field(Object o, String cls, String name) throws Exception {
        Class<?> c = o.getClass();
        String n = mem(cls, name);
        while (c != null) {
            try { Field f = c.getDeclaredField(n); f.setAccessible(true); return f.get(o); }
            catch (NoSuchFieldException e) { c = c.getSuperclass(); }
        }
        throw new NoSuchFieldException(cls + "." + name);
    }
    static float f(Object o, String cls, String name) throws Exception { return ((Number) field(o, cls, name)).floatValue(); }

    static final String MP = "net.minecraft.client.model.geom.ModelPart";

    static void part(StringBuilder sb, Object p) throws Exception {
        sb.append("{\"pose\":[");
        String[] fs = {"x", "y", "z", "xRot", "yRot", "zRot", "xScale", "yScale", "zScale"};
        for (int i = 0; i < fs.length; i++) {
            float v;
            try { v = f(p, MP, fs[i]); } catch (NoSuchFieldException e) { v = 1.0f; }
            if (i > 0) sb.append(',');
            sb.append(v);
        }
        sb.append("],\"visible\":").append(field(p, MP, "visible"));
        sb.append(",\"polys\":[");
        boolean first = true;
        for (Object cube : (List<?>) field(p, MP, "cubes")) {
            Object[] polys = (Object[]) field(cube, MP + "$Cube", "polygons");
            for (Object poly : polys) {
                if (!first) sb.append(',');
                first = false;
                Object[] verts = (Object[]) field(poly, MP + "$Polygon", "vertices");
                sb.append('[');
                for (int i = 0; i < verts.length; i++) {
                    Object v = verts[i];
                    float x, y, z;
                    try {
                        x = f(v, MP + "$Vertex", "x"); y = f(v, MP + "$Vertex", "y"); z = f(v, MP + "$Vertex", "z");
                    } catch (NoSuchFieldException e) { // <=1.21.x: Vector3f pos (pixels)
                        Object pos = field(v, MP + "$Vertex", "pos");
                        x = ((Number) pos.getClass().getField("x").get(pos)).floatValue();
                        y = ((Number) pos.getClass().getField("y").get(pos)).floatValue();
                        z = ((Number) pos.getClass().getField("z").get(pos)).floatValue();
                    }
                    if (i > 0) sb.append(',');
                    sb.append('[').append(x).append(',').append(y).append(',').append(z).append(',')
                      .append(f(v, MP + "$Vertex", "u")).append(',').append(f(v, MP + "$Vertex", "v")).append(']');
                }
                sb.append(']');
            }
        }
        sb.append("],\"children\":{");
        first = true;
        for (Map.Entry<String, ?> e : ((Map<String, ?>) field(p, MP, "children")).entrySet()) {
            if (!first) sb.append(',');
            first = false;
            sb.append('"').append(e.getKey()).append("\":");
            part(sb, e.getValue());
        }
        sb.append("}}");
    }

    public static void main(String[] args) throws Exception {
        if (args.length > 1) loadMappings(args[1]);
        try { // some versions need the registries up for a few layers
            Class<?> sc = cls("net.minecraft.SharedConstants");
            sc.getMethod(mem("net.minecraft.SharedConstants", "tryDetectVersion()")).invoke(null);
            Class<?> bs = cls("net.minecraft.server.Bootstrap");
            bs.getMethod(mem("net.minecraft.server.Bootstrap", "bootStrap()")).invoke(null);
        } catch (Throwable t) { System.err.println("bootstrap skipped: " + t); }
        Class<?> ld = cls("net.minecraft.client.model.geom.LayerDefinitions");
        Map<?, ?> roots = (Map<?, ?>) ld.getMethod(mem("net.minecraft.client.model.geom.LayerDefinitions", "createRoots()")).invoke(null);
        String LL = "net.minecraft.client.model.geom.ModelLayerLocation";
        String LD = "net.minecraft.client.model.geom.builders.LayerDefinition";
        StringBuilder sb = new StringBuilder("{");
        boolean first = true;
        TreeMap<String, Object> sorted = new TreeMap<>();
        for (Map.Entry<?, ?> e : roots.entrySet()) {
            Object loc = e.getKey();
            String id = String.valueOf(field(loc, LL, "model")) + "#" + field(loc, LL, "layer");
            sorted.put(id, e.getValue());
        }
        int n = 0;
        for (Map.Entry<String, Object> e : sorted.entrySet()) {
            Object def = e.getValue();
            Object root;
            try { root = def.getClass().getMethod(mem(LD, "bakeRoot()")).invoke(def); }
            catch (Throwable t) { System.err.println("skip " + e.getKey() + ": " + t); continue; }
            Object mat = field(def, LD, "material");
            int tw = ((Number) field(mat, "net.minecraft.client.model.geom.builders.MaterialDefinition", "xTexSize")).intValue();
            int th = ((Number) field(mat, "net.minecraft.client.model.geom.builders.MaterialDefinition", "yTexSize")).intValue();
            if (!first) sb.append(",\n");
            first = false;
            sb.append('"').append(e.getKey()).append("\":{\"tex\":[").append(tw).append(',').append(th).append("],\"root\":");
            part(sb, root);
            sb.append('}');
            n++;
        }
        sb.append("}\n");
        Files.writeString(Paths.get(args[0]), sb.toString());
        System.err.println("dumped " + n + " layers");
    }
}
