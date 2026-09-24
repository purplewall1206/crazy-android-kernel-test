import java.util.Formatter;

public class HelloFmt {
    public static void main(String[] args) throws Exception {
        System.err.printf("[hello] x=%d y=%.3f%n", 1, 2.5);
        System.err.println("[hello] done");
    }
}
