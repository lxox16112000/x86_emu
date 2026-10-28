// =====================================================================
// main.cpp - Mini x86-64 Emulator, Version 3
//
// Neu in Version 3:
//   - Speicherzugriffe: mov [rbx], rax / mov rcx, [rbx] / lea
//   - Stack: push, pop, call, ret (rsp zeigt auf den Stack)
//   - Befehle mit 8-Bit-Konstante: add/sub/cmp r/m64, imm8
//   - Displacements: [rbx+8], [rbx-4]
//
// Nicht unterstützt (kommt später): SIB-Byte, RIP-relative Adressen
// =====================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static const char* REG_NAMES[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
};

// ---------------------------------------------------------------------
// CPU-Zustand
// ---------------------------------------------------------------------
struct CPU {
    uint64_t regs[16] = {};   // 16 Register, rsp ist regs[4]
    uint64_t rip = 0;
    bool zf = false;
    bool sf = false;
    bool halted = false;
    std::vector<uint8_t> mem;
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "Fehler: %s\n", msg.c_str());
    std::exit(1);
}

// ---------------------------------------------------------------------
// Speicherzugriff mit Grenzprüfung
// ---------------------------------------------------------------------
void checkAddr(const CPU& c, uint64_t addr, uint64_t size) {
    if (addr + size > c.mem.size()) {
        die("Speicherzugriff außerhalb des Speichers bei Adresse " + std::to_string(addr));
    }
}

uint8_t read8(const CPU& c, uint64_t addr) {
    checkAddr(c, addr, 1);
    return c.mem[addr];
}

uint64_t read64(const CPU& c, uint64_t addr) {
    checkAddr(c, addr, 8);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)c.mem[addr + i] << (8 * i);
    }
    return v;
}

void write64(CPU& c, uint64_t addr, uint64_t v) {
    checkAddr(c, addr, 8);
    for (int i = 0; i < 8; i++) {
        c.mem[addr + i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

// ---------------------------------------------------------------------
// Befehle lesen (fetch) und RIP weiterschieben
// ---------------------------------------------------------------------
uint8_t fetch8(CPU& c) {
    uint8_t v = read8(c, c.rip);
    c.rip++;
    return v;
}

int32_t fetch32(CPU& c) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v |= (uint32_t)fetch8(c) << (8 * i);
    }
    return (int32_t)v;
}

uint64_t fetch64(CPU& c) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)fetch8(c) << (8 * i);
    }
    return v;
}

void setFlags(CPU& c, uint64_t result) {
    c.zf = (result == 0);
    c.sf = (result >> 63) & 1;
}

void trace(uint64_t addr, const std::string& text) {
    std::printf("  0x%04llx  %s\n", (unsigned long long)addr, text.c_str());
}

// ---------------------------------------------------------------------
// ModRM-Operand: entweder ein Register oder eine Speicheradresse
// ---------------------------------------------------------------------
struct Operand {
    int reg = 0;          // Register-Feld (mit REX.R)
    bool isMem = false;   // true = Speicher, false = Register
    int rm = 0;           // Register-Nummer, wenn kein Speicher
    int base = 0;         // Basisregister, wenn Speicher
    int64_t disp = 0;     // Displacement, wenn Speicher
    uint64_t addr = 0;    // berechnete Adresse, wenn Speicher
};

// Liest das ModRM-Byte (plus Displacement, falls nötig) und berechnet den Operanden
Operand decodeModRM(CPU& c, uint8_t rex) {
    bool rexR = (rex & 0x04) != 0;
    bool rexB = (rex & 0x01) != 0;

    uint8_t m = fetch8(c);
    int mod = m >> 6;
    int rmLow = m & 7;

    Operand o;
    o.reg = ((m >> 3) & 7) | (rexR ? 8 : 0);

    if (mod == 3) {                       // Register
        o.isMem = false;
        o.rm = rmLow | (rexB ? 8 : 0);
        return o;
    }

    if (rmLow == 4) die("SIB-Byte wird in dieser Version noch nicht unterstützt");
    if (rmLow == 5 && mod == 0) die("RIP-relative Adressierung wird noch nicht unterstützt");

    o.isMem = true;
    o.base = rmLow | (rexB ? 8 : 0);

    if (mod == 1) {
        o.disp = (int8_t)fetch8(c);       // 8-Bit-Displacement
    } else if (mod == 2) {
        o.disp = fetch32(c);              // 32-Bit-Displacement
    }
    o.addr = c.regs[o.base] + (uint64_t)o.disp;
    return o;
}

uint64_t getOperand(const CPU& c, const Operand& o) {
    return o.isMem ? read64(c, o.addr) : c.regs[o.rm];
}

void setOperand(CPU& c, const Operand& o, uint64_t value) {
    if (o.isMem) write64(c, o.addr, value);
    else         c.regs[o.rm] = value;
}

// Schreibweise für die Ablaufverfolgung, z. B. "[rbx+8]"
std::string operandName(const Operand& o) {
    if (!o.isMem) return REG_NAMES[o.rm];
    std::string s = std::string("[") + REG_NAMES[o.base];
    if (o.disp > 0)  s += "+" + std::to_string(o.disp);
    if (o.disp < 0)  s += std::to_string(o.disp);
    return s + "]";
}

// ---------------------------------------------------------------------
// Führt genau einen Befehl aus
// ---------------------------------------------------------------------
void step(CPU& c) {
    uint64_t start = c.rip;

    uint8_t rex = 0;
    uint8_t op = fetch8(c);
    if (op >= 0x40 && op <= 0x4F) {
        rex = op;
        op = fetch8(c);
    }
    bool rexB = (rex & 0x01) != 0;

    switch (op) {

    case 0x01: { // ADD r/m64, r64
        Operand o = decodeModRM(c, rex);
        uint64_t res = getOperand(c, o) + c.regs[o.reg];
        setOperand(c, o, res);
        setFlags(c, res);
        trace(start, "add " + operandName(o) + ", " + REG_NAMES[o.reg]);
        break;
    }

    case 0x29: { // SUB r/m64, r64
        Operand o = decodeModRM(c, rex);
        uint64_t res = getOperand(c, o) - c.regs[o.reg];
        setOperand(c, o, res);
        setFlags(c, res);
        trace(start, "sub " + operandName(o) + ", " + REG_NAMES[o.reg]);
        break;
    }

    case 0x39: { // CMP r/m64, r64  (wie SUB, aber ohne Speichern)
        Operand o = decodeModRM(c, rex);
        uint64_t res = getOperand(c, o) - c.regs[o.reg];
        setFlags(c, res);
        trace(start, "cmp " + operandName(o) + ", " + REG_NAMES[o.reg]);
        break;
    }

    case 0x83: { // Gruppe: ADD/SUB/CMP r/m64, imm8 (Auswahl über das reg-Feld)
        Operand o = decodeModRM(c, rex);
        int64_t imm = (int8_t)fetch8(c);
        uint64_t dst = getOperand(c, o);
        uint64_t src = (uint64_t)imm;
        int sub = o.reg & 7;

        if (sub == 0) {          // ADD
            uint64_t res = dst + src;
            setOperand(c, o, res);
            setFlags(c, res);
            trace(start, "add " + operandName(o) + ", " + std::to_string(imm));
        } else if (sub == 5) {   // SUB
            uint64_t res = dst - src;
            setOperand(c, o, res);
            setFlags(c, res);
            trace(start, "sub " + operandName(o) + ", " + std::to_string(imm));
        } else if (sub == 7) {   // CMP
            setFlags(c, dst - src);
            trace(start, "cmp " + operandName(o) + ", " + std::to_string(imm));
        } else {
            die("Unbekannte 0x83-Erweiterung bei Adresse " + std::to_string(start));
        }
        break;
    }

    case 0x89: { // MOV r/m64, r64  -> Speicher oder Register = Register
        Operand o = decodeModRM(c, rex);
        setOperand(c, o, c.regs[o.reg]);
        trace(start, "mov " + operandName(o) + ", " + REG_NAMES[o.reg]);
        break;
    }

    case 0x8B: { // MOV r64, r/m64  -> Register = Speicher oder Register
        Operand o = decodeModRM(c, rex);
        c.regs[o.reg] = getOperand(c, o);
        trace(start, std::string("mov ") + REG_NAMES[o.reg] + ", " + operandName(o));
        break;
    }

    case 0x8D: { // LEA r64, [m]  -> Adresse berechnen, NICHT lesen
        Operand o = decodeModRM(c, rex);
        if (!o.isMem) die("lea braucht einen Speicheroperanden");
        c.regs[o.reg] = o.addr;
        trace(start, std::string("lea ") + REG_NAMES[o.reg] + ", " + operandName(o));
        break;
    }

    case 0xC7: { // MOV r/m64, imm32
        Operand o = decodeModRM(c, rex);
        int32_t imm = fetch32(c);
        setOperand(c, o, (uint64_t)(int64_t)imm);
        trace(start, "mov " + operandName(o) + ", " + std::to_string(imm));
        break;
    }

    case 0x50: case 0x51: case 0x52: case 0x53:  // PUSH r64
    case 0x54: case 0x55: case 0x56: case 0x57: {
        int reg = (op - 0x50) | (rexB ? 8 : 0);
        c.regs[4] -= 8;                          // rsp zuerst verkleinern
        write64(c, c.regs[4], c.regs[reg]);
        trace(start, std::string("push ") + REG_NAMES[reg]);
        break;
    }

    case 0x58: case 0x59: case 0x5A: case 0x5B:  // POP r64
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        int reg = (op - 0x58) | (rexB ? 8 : 0);
        uint64_t v = read64(c, c.regs[4]);
        c.regs[4] += 8;
        c.regs[reg] = v;
        trace(start, std::string("pop ") + REG_NAMES[reg]);
        break;
    }

    case 0xE8: { // CALL rel32  -> Rücksprungadresse auf den Stack, dann springen
        int32_t rel = fetch32(c);
        c.regs[4] -= 8;
        write64(c, c.regs[4], c.rip);
        c.rip += (int64_t)rel;
        trace(start, "call (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0xC3: { // RET  -> Rücksprungadresse vom Stack holen
        c.rip = read64(c, c.regs[4]);
        c.regs[4] += 8;
        trace(start, "ret");
        break;
    }

    case 0xB8: case 0xB9: case 0xBA: case 0xBB:  // MOV r64, imm64
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        int reg = (op - 0xB8) | (rexB ? 8 : 0);
        uint64_t imm = fetch64(c);
        c.regs[reg] = imm;
        trace(start, std::string("mov ") + REG_NAMES[reg] + ", " + std::to_string(imm));
        break;
    }

    case 0xEB: { // JMP rel8
        int8_t rel = (int8_t)fetch8(c);
        c.rip += rel;
        trace(start, "jmp (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0x74: { // JE rel8
        int8_t rel = (int8_t)fetch8(c);
        if (c.zf) c.rip += rel;
        trace(start, "je (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0x75: { // JNE rel8
        int8_t rel = (int8_t)fetch8(c);
        if (!c.zf) c.rip += rel;
        trace(start, "jne (relativ " + std::to_string(rel) + ")");
        break;
    }

    case 0xF4: // HLT
        c.halted = true;
        trace(start, "hlt");
        break;

    default:
        die("Unbekannter Befehl 0x" + std::to_string(op) + " bei Adresse " + std::to_string(start));
    }
}

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
    cpu.mem.resize(4096, 0);

    // Testprogramm (Adresse: Befehl)
    //  0: mov rsp, 0x800          Stack beginnt bei 0x800 und wächst nach unten
    //  7: mov rbx, 0x100          Zeiger auf eine Speicherstelle
    // 14: mov rax, 42
    // 21: mov [rbx], rax          Speicher[0x100] = 42
    // 24: mov rcx, [rbx]          rcx = 42
    // 27: push rcx
    // 28: mov rdx, 0
    // 35: pop rdx                 rdx = 42
    // 36: call funktion           springt nach 42
    // 41: hlt                     kehrt von der Funktion hierher zurück
    // 42: funktion: add rdx, 8    rdx = 50
    // 46: ret
    std::vector<uint8_t> program = {
        0x48, 0xC7, 0xC4, 0x00, 0x08, 0x00, 0x00,
        0x48, 0xC7, 0xC3, 0x00, 0x01, 0x00, 0x00,
        0x48, 0xC7, 0xC0, 0x2A, 0x00, 0x00, 0x00,
        0x48, 0x89, 0x03,
        0x48, 0x8B, 0x0B,
        0x51,
        0x48, 0xC7, 0xC2, 0x00, 0x00, 0x00, 0x00,
        0x5A,
        0xE8, 0x01, 0x00, 0x00, 0x00,
        0xF4,
        0x48, 0x83, 0xC2, 0x08,
        0xC3
    };

    for (size_t i = 0; i < program.size(); i++) {
        cpu.mem[i] = program[i];
    }

    cpu.rip = 0;
    std::printf("--- Ablaufverfolgung ---\n");
    while (!cpu.halted) {
        step(cpu);
    }

    dumpState(cpu);

    // Automatischer Test
    bool ok = cpu.regs[0] == 42           // rax
           && cpu.regs[1] == 42           // rcx
           && cpu.regs[2] == 50           // rdx
           && cpu.regs[3] == 0x100        // rbx
           && cpu.regs[4] == 0x800        // rsp (Stack ausgeglichen)
           && read64(cpu, 0x100) == 42;   // Speicher

    std::printf("\n%s\n", ok ? "TEST BESTANDEN" : "TEST FEHLGESCHLAGEN");
    return ok ? 0 : 1;
}
