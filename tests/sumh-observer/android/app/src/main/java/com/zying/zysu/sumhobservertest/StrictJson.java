package com.zying.zysu.sumhobservertest;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Bounded JSON data parser. Duplicate keys, non-finite numbers and lone surrogates are rejected. */
final class StrictJson {
    private final String text;
    private int pos;
    private int nodes;
    private StrictJson(String text) { this.text = text; }
    static String utf8(byte[] bytes, int limit) {
        if (bytes.length > limit) throw new IllegalArgumentException("Input exceeds byte limit");
        try { return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString(); }
        catch (CharacterCodingException e) { throw new IllegalArgumentException("Invalid UTF-8", e); }
    }
    static Object parse(byte[] bytes, int limit) {
        StrictJson parser = new StrictJson(utf8(bytes, limit));
        Object result = parser.value(0);
        parser.space();
        if (parser.pos != parser.text.length()) throw parser.bad();
        return result;
    }
    static Map<String, Object> object(Object value) {
        if (!(value instanceof Map<?, ?>)) throw new IllegalArgumentException("Expected object");
        @SuppressWarnings("unchecked") Map<String, Object> map = (Map<String, Object>) value;
        return map;
    }
    static String string(Object value) {
        if (!(value instanceof String)) throw new IllegalArgumentException("Expected string");
        return (String) value;
    }
    private IllegalArgumentException bad() { return new IllegalArgumentException("Invalid JSON at " + pos); }
    private void space() { while (pos < text.length() && " \t\r\n".indexOf(text.charAt(pos)) >= 0) pos++; }
    private boolean take(char c) { space(); if (pos < text.length() && text.charAt(pos) == c) { pos++; return true; } return false; }
    private Object value(int depth) {
        if (depth > 24 || ++nodes > 20000) throw bad();
        space();
        if (pos == text.length()) throw bad();
        char c = text.charAt(pos);
        if (c == '"') return quoted();
        if (take('{')) {
            Map<String, Object> map = new LinkedHashMap<>();
            if (take('}')) return map;
            do { space(); String key = quoted(); if (!take(':') || map.containsKey(key)) throw bad(); map.put(key, value(depth + 1)); } while (take(','));
            if (!take('}')) throw bad(); return map;
        }
        if (take('[')) {
            List<Object> list = new ArrayList<>();
            if (take(']')) return list;
            do { list.add(value(depth + 1)); } while (take(','));
            if (!take(']')) throw bad(); return list;
        }
        for (String literal : new String[]{"true", "false", "null"}) {
            if (text.startsWith(literal, pos)) { pos += literal.length(); return literal.equals("null") ? null : literal.equals("true"); }
        }
        int start = pos;
        if (pos < text.length() && text.charAt(pos) == '-') pos++;
        if (pos >= text.length()) throw bad();
        if (text.charAt(pos) == '0') pos++;
        else { if (text.charAt(pos) < '1' || text.charAt(pos) > '9') throw bad(); while (pos < text.length() && digit(text.charAt(pos))) pos++; }
        if (pos < text.length() && text.charAt(pos) == '.') { pos++; int mark = pos; while (pos < text.length() && digit(text.charAt(pos))) pos++; if (pos == mark) throw bad(); }
        if (pos < text.length() && "eE".indexOf(text.charAt(pos)) >= 0) { pos++; if (pos < text.length() && "+-".indexOf(text.charAt(pos)) >= 0) pos++; int mark = pos; while (pos < text.length() && digit(text.charAt(pos))) pos++; if (pos == mark) throw bad(); }
        String token = text.substring(start, pos);
        if (!Double.isFinite(Double.parseDouble(token))) throw bad();
        return new NumberToken(token);
    }
    private static boolean digit(char c) { return c >= '0' && c <= '9'; }
    private String quoted() {
        if (pos == text.length() || text.charAt(pos++) != '"') throw bad();
        StringBuilder result = new StringBuilder(); boolean ended = false;
        while (pos < text.length()) {
            char c = text.charAt(pos++);
            if (c == '"') { ended = true; break; }
            if (c < 0x20) throw bad();
            if (c == '\\') {
                if (pos == text.length()) throw bad(); c = text.charAt(pos++);
                switch (c) {
                    case '"': case '\\': case '/': break;
                    case 'b': c = '\b'; break; case 'f': c = '\f'; break;
                    case 'n': c = '\n'; break; case 'r': c = '\r'; break; case 't': c = '\t'; break;
                    case 'u':
                        if (pos + 4 > text.length()) throw bad();
                        int code = 0;
                        for (int i = 0; i < 4; i++) { char h = text.charAt(pos++); int d = "0123456789abcdef".indexOf(Character.toLowerCase(h)); if (d < 0 || h > 127) throw bad(); code = (code << 4) | d; }
                        c = (char) code; break;
                    default: throw bad();
                }
            }
            result.append(c);
        }
        if (!ended) throw bad();
        for (int i = 0; i < result.length(); i++) {
            char c = result.charAt(i);
            if (Character.isHighSurrogate(c)) { if (++i >= result.length() || !Character.isLowSurrogate(result.charAt(i))) throw bad(); }
            else if (Character.isLowSurrogate(c)) throw bad();
        }
        return result.toString();
    }
    static final class NumberToken {
        final String text;
        NumberToken(String text) { this.text = text; }
    }
}
