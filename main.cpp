// =====================================================================
// main.cpp - Mini x86-64 Emulator, Version 2
//
// Was dieser Emulator kann:
//   - Register rax bis r15 (16 Stück, jeweils 64 Bit)
//   - Flags: ZF (Zero Flag) und SF (Sign Flag)
//   - Befehle: MOV, ADD, SUB, CMP, JMP, JE, JNE, HLT
//   - Nur Register als Operanden (kein Speicherzugriff über Adressen)
//
// Das Testprogramm berechnet:
//   rax = 5 + 7 = 12
//   rdx = 3 + 2 + 1 = 6 (mit einer Schleife)
// =====================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// Namen der 64-Bit-Register, für die Ausgabe
static const char* REG_NAMES[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

// ---------------------------------------------------------------------
// Der Zustand der CPU: Register, Befehlszeiger, Flags und Speicher
// ---------------------------------------------------------------------
struct CPU {
    uint64_t regs[16] = {};   // Die 16 allgemeinen Register
    uint64_t rip = 0;         // Adresse des nächsten Befehls
    bool zf = false;          // Zero Flag: Ergebnis war 0
    bool sf = false;          // Sign Flag: Ergebnis war negativ (oberstes Bit gesetzt)
    bool halted = false;      // Wird true, wenn HLT ausgeführt wurde
    std::vector<uint8_t> mem; // Der Speicher, Byte für Byte
};

// Bricht mit einer Fehlermeldung ab
[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "Fehler: %s\n", msg.c_str());
    std::exit(1);
}

// Liest ein Byte an einer Adresse, mit Prüfung der Grenzen
uint8_t read8(const CPU& c, uint64_t addr) {
    if (addr >= c.mem.size()) {
        die("Zugriff außerhalb des Speichers bei Adresse " + std::to_string(addr));
    }
    return c.mem[addr];
}

// Holt ein Byte an der aktuellen RIP-Position und erhöht RIP um 1
uint8_t fetch8(CPU& c) {
    return read8(c, c.rip++);
}

// Holt 4 Bytes (little endian), als vorzeichenbehaftete 32-Bit-Zahl
int32_t fetch32(CPU& c) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)fetch8(c) << (8 * i);
    }
    return (int32_t)v;
}

// Holt 8 Bytes (little endian), als 64-Bit-Zahl
uint64_t fetch64(CPU& c) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)fetch8(c) << (8 * i);
    }
    return v;
}

// Setzt ZF und SF anhand eines Rechenergebnisses
void setFlags(CPU& c, uint64_t result) {
    c.zf = (result == 0);
    c.sf = (result >> 63) & 1;
}

// Gibt eine Zeile der Ablaufverfolgung aus
void trace(uint64_t addr, const std::string& text) {
    std::printf("  0x%04llx  %s\n", (unsigned long long)addr, text.c_str());
}

// ---------------------------------------------------------------------
// Führt genau einen Befehl aus
// ---------------------------------------------------------------------
void step(CPU& c) {
    uint64_t start = c.rip;   // Merken, wo der Befehl anfängt (für die Ausgabe)

    // 1. REX-Prefix lesen (0x40 bis 0x4F). Er erweitert die Register auf r8 bis r15.
    //    Bit 3 (W): 64-Bit-Operation, Bit 2 (R): Erweiterung für reg,
    //    Bit 0 (B): Erweiterung für rm.
    uint8_t rex = 0;
    uint8_t op = fetch8(c);
    if (op >= 0x40 && op <= 0x4F) {
        rex = op;
        op = fetch8(c);
    }
    bool rexR = (rex & 0x04) != 0;
    bool rexB = (rex & 0x01) != 0;

    // 2. Hilfsfunktion: ModRM-Byte lesen.
    //    ModRM hat drei Teile: mod (2 Bit), reg (3 Bit), rm (3 Bit).
    //    In dieser Version ist nur mod = 11 erlaubt, also Register statt Speicher.
    auto readModRM = [&](int& reg, int& rm) {
        uint8_t m = fetch8(c);
        if ((m >> 6) != 3) {
            die("Speicheroperanden (mod != 11) werden in dieser Version noch nicht unterstützt");
        }
        reg = ((m >> 3) & 7) | (rexR ? 8 : 0);
        rm  = (m & 7)       | (rexB ? 8 : 0);
    };

    // 3. Den eigentlichen Befehl ausführen
    switch (op) {

    case 0x01: { // ADD r/m64, r64  ->  rm = rm + reg
        int reg, rm;
        readModRM(reg, rm);
        c.regs[rm] += c.regs[reg];
        setFlags(c, c.regs[rm]);
        trace(start, std::string("add ") + REG_NAMES[rm] + ", " + REG_NAMES[reg]);
        break;
    }

    case 0x29: { // SUB r/m64, r64  ->  rm = rm - reg
        int reg, rm;
        readModRM(reg, rm);
        c.regs[rm] -= c.regs[reg];
        setFlags(c, c.regs[rm]);
        trace(start, std::string("sub ") + REG_NAMES[rm] + ", " + REG_NAMES[reg]);
        break;
    }

    case 0x39: { // CMP r/m64, r64  ->  wie SUB, aber ohne das Ergebnis zu speichern
        int reg, rm;
        readModRM(reg, rm);
        uint64_t result = c.regs[rm] - c.regs[reg];
        setFlags(c, result);
        trace(start, std::string("cmp ") + REG_NAMES[rm] + ", " + REG_NAMES[reg]);
        break;
    }

    case 0x89: { // MOV r/m64, r64  ->  rm = reg
        int reg, rm;
        readModRM(reg, rm);
        c.regs[rm] = c.regs[reg];
        trace(start, std::string("mov ") + REG_NAMES[rm] + ", " + REG_NAMES[reg]);
        break;
    }

    case 0x8B: { // MOV r64, r/m64  ->  reg = rm
        int reg, rm;
        readModRM(reg, rm);
        c.regs[reg] = c.regs[rm];
        trace(start, std::string("mov ") + REG_NAMES[reg] + ", " + REG_NAMES[rm]);
        break;
    }

    case 0xC7: { // MOV r/m64, imm32  ->  rm = 32-Bit-Zahl (Vorzeichen wird auf 64 Bit erweitert)
        int reg, rm;
        readModRM(reg, rm);           // Das reg-Feld ist hier immer 0, wir ignorieren es
        int32_t imm = fetch32(c);
        c.regs[rm] = (uint64_t)(int64_t)imm;
        trace(start, std::string("mov ") + REG_NAMES[rm] + ", " + std::to_string(imm));
        break;
    }

    case 0xB8: case 0xB9: case 0xBA: case 0xBB:  // MOV r64, imm64
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int reg = (op - 0xB8) | (rexB ? 8 : 0);  // Das Register steckt direkt im Opcode
        uint64_t imm = fetch64(c);
        c.regs[reg] = imm;
        trace(start, std::string("mov ") + REG_NAMES[reg] + ", " + std::to_string(imm));
        break;
    }

    case 0xEB: { // JMP rel8  ->  unbedingter Sprung
        int8_t rel = (int8_t)fetch8(c);
        c.rip += rel;   // RIP zeigt schon auf den nächsten Befehl
        trace(start, "jmp (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0x74: { // JE rel8  ->  springe, wenn ZF gesetzt ist (gleich)
        int8_t rel = (int8_t)fetch8(c);
        if (c.zf) c.rip += rel;
        trace(start, "je (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0x75: { // JNE rel8  ->  springe, wenn ZF NICHT gesetzt ist (ungleich)
        int8_t rel = (int8_t)fetch8(c);
        if (!c.zf) c.rip += rel;
        trace(start, "jne (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0xF4: // HLT  ->  Programm anhalten
        c.halted = true;
        trace(start, "hlt");
        break;

    default:
        die("Unbekannter Befehl 0x" + std::to_string(op) + " bei Adresse " + std::to_string(start));
    }
}

// Gibt alle Register und Flags aus
void dumpState(const CPU& c) {
    std::printf("\n--- Endzustand ---\n");
    for (int i = 0; i < 16; i++) {
        std::printf("%-4s = %llu\n", REG_NAMES[i], (unsigned long long)c.regs[i]);
    }
    std::printf("ZF = %d, SF = %d, RIP = 0x%llx\n",
                c.zf ? 1 : 0, c.sf ? 1 : 0, (unsigned long long)c.rip);
}

int main() {
    CPU cpu;
    cpu.mem.resize(4096, 0);   // 4 KB Speicher, am Anfang mit Nullen gefüllt

    // Das Testprogramm als Maschinencode. Die Kommentare zeigen die Adressen
    // und den Assembler-Befehl, damit du es nachvollziehen kannst.
    std::vector<uint8_t> program = {
        // 0:  mov rax, 5
        0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00,
        // 7:  mov rbx, 7
        0x48, 0xC7, 0xC3, 0x07, 0x00, 0x00, 0x00,
        // 14: add rax, rbx          -> rax = 12
        0x48, 0x01, 0xD8,
        // 17: mov rcx, 3            (Schleifenzähler)
        0x48, 0xC7, 0xC1, 0x03, 0x00, 0x00, 0x00,
        // 24: mov rdx, 0            (Summe)
        0x48, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00,
        // 31: mov rsi, 1            (Schrittweite)
        0x48, 0xC7, 0xC6, 0x01, 0x00, 0x00, 0x00,
        // 38: schleife: add rdx, rcx
        0x48, 0x01, 0xCA,
        // 41: sub rcx, rsi
        0x48, 0x29, 0xF1,
        // 44: jne schleife          (0xF8 = -8, zurück auf Adresse 38)
        0x75, 0xF8,
        // 46: hlt
        0xF4
    };

    for (size_t i = 0; i < program.size(); i++) {
        cpu.mem[i] = program[i];
    }

    // Die Ablaufverfolgung zeigt jeden ausgeführten Befehl
    std::printf("--- Ablaufverfolgung ---\n");
    while (!cpu.halted) {
        step(cpu);
    }

    dumpState(cpu);

    // Automatischer Test: Stimmen die erwarteten Ergebnisse?
    bool ok = cpu.regs[0] == 12   // rax
           && cpu.regs[3] == 7    // rbx
           && cpu.regs[2] == 6    // rdx
           && cpu.regs[1] == 0    // rcx
           && cpu.regs[6] == 1;   // rsi

    std::printf("\n%s\n", ok ? "TEST BESTANDEN" : "TEST FEHLGESCHLAGEN");
    return ok ? 0 : 1;
}
