package stardataparadise.exported;

import java.math.BigDecimal;
import java.math.RoundingMode;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.time.LocalDate;
import java.time.LocalTime;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Supplier;

/** Value and control primitives for generated procedures and functions. */
public final class RoutineRuntime {
    private RoutineRuntime() {}

    public record Site(long offset, int line, int column) {}

    record Type(String name, int precision, int scale) {}

    /** An SQL instant with its original offset and nanoseconds. */
    public record Timestamp(long seconds, int nanos, int offsetMinutes) {
        public Timestamp {
            if (nanos < 0 || nanos > 999_999_999 || offsetMinutes < -840 || offsetMinutes > 840) {
                throw new IllegalArgumentException("Invalid timestamp");
            }
        }
    }

    public record Field(String name, Object value) {
        public Field {
            Objects.requireNonNull(name, "name");
            if (name.isEmpty() || name.indexOf(0) >= 0 || utf8Bytes(name).length > 128) {
                throw new IllegalArgumentException("Invalid record field name");
            }
            value = copy(value);
        }

        @Override public Object value() {
            return copy(value);
        }
    }

    public record RecordValue(List<Field> fields) {
        public RecordValue {
            fields = List.copyOf(fields);
            var names = new HashSet<String>();
            if (fields.size() > 128) throw new IllegalArgumentException("Too many record fields");
            for (var field : fields) {
                if (!names.add(field.name())) throw new IllegalArgumentException("Duplicate record field");
            }
        }
    }

    public record CollectionValue(List<Object> items) {
        public CollectionValue {
            if (items.size() > 128) throw new IllegalArgumentException("Too many collection items");
            items = copyItems(items);
        }

        @Override public List<Object> items() {
            return copyItems(items);
        }
    }

    public record RoutineResult(Object returned, Map<String, Object> out) {
        public RoutineResult {
            returned = copy(returned);
            out = copyOutputs(out);
        }

        @Override public Object returned() { return copy(returned); }
        @Override public Map<String, Object> out() { return copyOutputs(out); }
    }

    public static final class Failure extends RuntimeException {
        private static final long serialVersionUID = 1L;
        private final int code;
        private final String plException;
        private final Site site;

        private Failure(int code, String plException, Site site, String message) {
            super(message);
            this.code = code;
            this.plException = plException;
            this.site = site;
        }

        public int code() { return code; }
        public String plException() { return plException; }
        public Site site() { return site; }
    }

    static final class Return extends RuntimeException {
        private static final long serialVersionUID = 1L;
        final Object value;

        Return(Object value) {
            super(null, null, false, false);
            this.value = value;
        }
    }

    static final class Context {
        private static final int STEPS = 100_000;
        private static final int DEPTH = 32;
        private static final int MEMORY = 1024 * 1024;

        private final long started = System.nanoTime();
        private int steps;
        private int depth;
        private int live;
        Failure activeException;

        void step(Site site, int lexicalDepth) {
            if (lexicalDepth > DEPTH) throw limit(site, "PL/SQL lexical depth exceeded");
            if (steps == STEPS) throw limit(site, "PL/SQL instruction budget exceeded");
            if (System.nanoTime() - started >= 2_000_000_000L) throw limit(site, "PL/SQL time budget exceeded");
            ++steps;
        }

        Frame enter(Type[] slots, Site site) {
            if (depth == DEPTH) throw limit(site, "PL/SQL call depth exceeded");
            step(site, 0);
            int bytes = slots.length * 64;
            claim(bytes, site);
            ++depth;
            return new Frame(this, slots, bytes);
        }

        Object evaluate(Site site, int lexicalDepth, Supplier<Object> operation) {
            step(site, lexicalDepth);
            return operation.get();
        }

        private void claim(int bytes, Site site) {
            if (bytes > MEMORY - live) throw limit(site, "PL/SQL memory budget exceeded");
            live += bytes;
        }
    }

    static final class Frame implements AutoCloseable {
        private final Context context;
        private final Type[] slots;
        private final Object[] cells;
        private int bytes;

        private Frame(Context context, Type[] slots, int bytes) {
            this.context = context;
            this.slots = slots;
            this.cells = new Object[slots.length];
            this.bytes = bytes;
        }

        Object get(int slot) { return cells[slot]; }

        void put(int slot, Object value, Site site) {
            var fitted = fit(value, slots[slot], site);
            var added = valueBytes(fitted, site) - valueBytes(cells[slot], site);
            if (added > 0) context.claim(added, site);
            else context.live += added;
            bytes += added;
            cells[slot] = fitted;
        }

        @Override public void close() {
            context.live -= bytes;
            --context.depth;
        }
    }

    static Object copy(Object value) {
        return value instanceof byte[] bytes ? bytes.clone() : value;
    }

    private static List<Object> copyItems(List<Object> source) {
        var result = new ArrayList<Object>(source.size());
        for (Object value : source) result.add(copy(value));
        return Collections.unmodifiableList(result);
    }

    private static Map<String, Object> copyOutputs(Map<String, Object> source) {
        var result = new LinkedHashMap<String, Object>();
        source.forEach((key, value) -> result.put(key, copy(value)));
        return Collections.unmodifiableMap(result);
    }

    private static byte[] utf8Bytes(String text) {
        try {
            var encoded = StandardCharsets.UTF_8.newEncoder().encode(java.nio.CharBuffer.wrap(text));
            var bytes = new byte[encoded.remaining()];
            encoded.get(bytes);
            return bytes;
        } catch (CharacterCodingException invalid) {
            throw new IllegalArgumentException("Text must contain Unicode scalar values", invalid);
        }
    }

    static String utf8(String base64) {
        try {
            return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(Base64.getDecoder().decode(base64))).toString();
        } catch (CharacterCodingException invalid) {
            throw new IllegalArgumentException("Invalid generated UTF-8 literal", invalid);
        }
    }

    private static int valueBytes(Object value, Site site) {
        try {
            if (value instanceof String text) return utf8Bytes(text).length;
            if (value instanceof byte[] bytes) return bytes.length;
            if (value instanceof BigDecimal fixed) return fixed.unscaledValue().abs().toString().length();
            if (value instanceof RecordValue || value instanceof CollectionValue) {
                int[] nodes = new int[1];
                int bytes = compoundBytes(value, site, 0, nodes);
                if (bytes > 65_536) throw limit(site, "PL/SQL compound byte budget exceeded");
                return bytes;
            }
            return 0;
        } catch (IllegalArgumentException invalid) {
            throw badValue(site, "Text must contain Unicode scalar values");
        }
    }

    // These estimates account for encoded typed compound fields plus their live payload.
    private static int compoundBytes(Object value, Site site, int depth, int[] nodes) {
        if (++nodes[0] > 2048) throw limit(site, "PL/SQL compound node limit exceeded");
        int size = 16;
        if (value instanceof RecordValue record) {
            if (depth >= 8) throw limit(site, "PL/SQL compound nesting depth exceeded");
            for (var field : record.fields()) {
                size += utf8Bytes(field.name()).length + 12 + compoundBytes(field.value, site, depth + 1, nodes);
                if (size > 65_536) throw limit(site, "PL/SQL compound byte budget exceeded");
            }
            return size;
        }
        if (value instanceof CollectionValue items) {
            if (depth >= 8) throw limit(site, "PL/SQL compound nesting depth exceeded");
            for (var item : items.items) {
                size += compoundBytes(item, site, depth + 1, nodes);
                if (size > 65_536) throw limit(site, "PL/SQL compound byte budget exceeded");
            }
            return size;
        }
        if (value == null || value instanceof Long || value instanceof Boolean || value instanceof LocalDate
                || value instanceof LocalTime || value instanceof Timestamp) return size;
        if (value instanceof String text) return size + utf8Bytes(text).length;
        if (value instanceof byte[] blob) {
            if (blob.length > 32_768) throw limit(site, "PL/SQL compound byte budget exceeded");
            return size + blob.length * 2;
        }
        if (value instanceof BigDecimal number && number.scale() >= 0 && number.scale() <= 38
                && number.precision() <= 38) return size + number.precision();
        throw badValue(site, "Invalid typed PL/SQL compound value");
    }

    static Object fit(Object value, Type type, Site site) {
        if (value == null) return null;
        return switch (type.name()) {
            case "BIGINT" -> integer(value, site);
            case "BOOLEAN" -> checked(value, Boolean.class, site);
            case "TEXT" -> checked(value, String.class, site);
            case "BLOB" -> checked(value, byte[].class, site);
            case "DATE" -> checked(value, LocalDate.class, site);
            case "TIME" -> checked(value, LocalTime.class, site);
            case "TIMESTAMP WITH TIME ZONE" -> checked(value, Timestamp.class, site);
            case "RECORD" -> checked(value, RecordValue.class, site);
            case "COLLECTION" -> checked(value, CollectionValue.class, site);
            case "DECIMAL" -> {
                BigDecimal number = decimal(value, site);
                if (number.precision() > 38 || number.scale() < 0 || number.scale() > 38) {
                    throw badValue(site, "Decimal outside the supported bounds");
                }
                BigDecimal result = number.setScale(type.scale(), RoundingMode.HALF_UP);
                if (result.precision() > type.precision()) {
                    throw badValue(site, "Rounded decimal exceeds its declared precision");
                }
                yield result;
            }
            default -> throw badValue(site, "Unsupported value type");
        };
    }

    private static <T> T checked(Object value, Class<T> expected, Site site) {
        if (!expected.isInstance(value)) throw badValue(site, "Value does not match its SQL type");
        return expected.cast(value);
    }

    static long integer(Object value, Site site) {
        if (value instanceof Long number) return number;
        throw badValue(site, "Expected a non-NULL BIGINT");
    }

    private static BigDecimal decimal(Object value, Site site) {
        if (value instanceof BigDecimal number) return number;
        return BigDecimal.valueOf(integer(value, site));
    }

    static Object negate(Object value, Site site) {
        if (value == null) return null;
        if (value instanceof BigDecimal decimal) return decimal.negate();
        try {
            return Math.negateExact(integer(value, site));
        } catch (ArithmeticException overflow) {
            throw badValue(site, "Signed BIGINT arithmetic overflow");
        }
    }

    static Object arithmetic(String operation, Object left, Object right, Site site) {
        if (left == null || right == null) return null;
        long a = integer(left, site), b = integer(right, site);
        try {
            return switch (operation) {
                case "add" -> Math.addExact(a, b);
                case "subtract" -> Math.subtractExact(a, b);
                case "multiply" -> Math.multiplyExact(a, b);
                case "divide" -> {
                    if (b == 0) throw new Failure(1012, "zero_divide", site, "Division by zero");
                    if (a == Long.MIN_VALUE && b == -1) throw new ArithmeticException();
                    yield a / b;
                }
                default -> throw badValue(site, "Unsupported arithmetic");
            };
        } catch (ArithmeticException overflow) {
            throw badValue(site, "Signed BIGINT arithmetic overflow");
        }
    }

    static Object compare(String operation, Object left, Object right, Site site) {
        if (left == null || right == null) return null;
        int order;
        if (left instanceof Long a && right instanceof Long b) order = Long.compare(a, b);
        else if (left instanceof Long || left instanceof BigDecimal) order = decimal(left, site).compareTo(decimal(right, site));
        else if (left instanceof Boolean a && right instanceof Boolean b) order = Boolean.compare(a, b);
        else if (left instanceof String a && right instanceof String b) order = Arrays.compareUnsigned(utf8Bytes(a), utf8Bytes(b));
        else if (left instanceof byte[] a && right instanceof byte[] b) order = Arrays.compareUnsigned(a, b);
        else if (left instanceof LocalDate a && right instanceof LocalDate b) order = a.compareTo(b);
        else if (left instanceof LocalTime a && right instanceof LocalTime b) order = a.compareTo(b);
        else if (left instanceof Timestamp a && right instanceof Timestamp b) {
            order = Long.compare(a.seconds(), b.seconds());
            if (order == 0) order = Integer.compare(a.nanos(), b.nanos());
        } else throw badValue(site, "Incompatible comparison operands");
        return switch (operation) {
            case "eq" -> order == 0;
            case "ne" -> order != 0;
            case "lt" -> order < 0;
            case "le" -> order <= 0;
            case "gt" -> order > 0;
            case "ge" -> order >= 0;
            default -> throw badValue(site, "Unsupported comparison");
        };
    }

    static boolean truth(Object value) { return Boolean.TRUE.equals(value); }

    static Object not(Object value) {
        return value == null ? null : !truth(value);
    }

    static Object and(Object left, Supplier<Object> right) {
        if (Boolean.FALSE.equals(left)) return false;
        Object candidate = right.get();
        if (Boolean.FALSE.equals(candidate)) return false;
        return left == null || candidate == null ? null : true;
    }

    static Object or(Object left, Supplier<Object> right) {
        if (Boolean.TRUE.equals(left)) return true;
        Object candidate = right.get();
        if (Boolean.TRUE.equals(candidate)) return true;
        return left == null || candidate == null ? null : false;
    }

    static Failure badValue(Site site, String message) { return new Failure(1012, "value_error", site, message); }

    static Failure programError(Site site) {
        return new Failure(1012, "program_error", site, "Function reached its end without returning a value");
    }

    static Failure invalidArgument(Site site, String message) { return new Failure(1012, null, site, message); }

    private static Failure limit(Site site, String message) { return new Failure(1014, null, site, message); }

    static void returnValue(Object value) {
        throw new Return(value);
    }

    static void throwException(String name, Site site, Context context) {
        throw raise(name, site, context);
    }

    private static Failure raise(String name, Site site, Context context) {
        if (!name.isEmpty()) return new Failure(1012, name, site, "PL/SQL routine raised an exception");
        if (context.activeException != null) return context.activeException;
        return new Failure(1004, null, site, "RAISE has no active exception");
    }
}
