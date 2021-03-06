// debugger symbol table

#ifndef __DEBUGGER_SYMBOLS_H__
#define __DEBUGGER_SYMBOLS_H__

#include <stdint.h>

#define SYM_MAX 32
#define STRINGY(x) STRINGY2(x)
#define STRINGY2(x) #x


#ifdef __cplusplus
extern "C" {
#endif

typedef void(*debug_outf_t)(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

typedef struct cpu_debug_t cpu_debug_t;

// a bit of doding about to allow C to access CPP
#ifdef __cplusplus

#include <map>
#include <string>

    class symbol_compare {
    public:
        bool operator()(const char *a, const char *b) const { return strcmp(a, b) < 0; };
    };

    //we need to sort backwards as we want upper_bound to return the lower address and favour address < the one we search for in addr near
    class symbol_addr_compare {
    public:
        bool operator()(uint32_t a, uint32_t b) const { return a > b; }
    };

    class symbol_entry {
    private:
        char *symbol;
        uint32_t addr;
    public:
        symbol_entry(const char *_symbol, uint32_t _addr) {
            symbol = (char *)malloc(strlen(_symbol) + 1);
            if (symbol) {
                strcpy(symbol, _symbol);
            }
            addr = _addr;
        }
        symbol_entry(const symbol_entry &) = delete;
        symbol_entry(symbol_entry &&other) {
            this->addr = other.addr;
            this->symbol = other.symbol;
            other.symbol = NULL;
        }
        ~symbol_entry() {
            if (symbol)
                free(symbol);
        }
        uint32_t getAddr() const { return addr; }
        const char *getSymbol() const { return symbol; }
    };

    class symbol_table {
    private:
        std::map<const char *, symbol_entry*, symbol_compare> map;
        std::multimap<uint32_t, symbol_entry*, symbol_addr_compare> byaddrmap;
    public:
        ~symbol_table();
        void add(const char *symbol, uint32_t addr);
        bool find_by_addr(uint32_t addr, const char *&ret) const;
        bool find_by_name(const char *name, uint32_t &ret) const;
        bool find_by_addr_near(uint32_t addr, uint32_t min, uint32_t max, uint32_t &addr_found, const char *&ret) const;
        int length() const { return map.size(); }

        void symbol_list(cpu_debug_t *cpu, debug_outf_t debug_outf) const;
    };
#else
    typedef struct symbol_table symbol_table;
#endif

#ifdef __cplusplus
    extern "C" {
#else
#include <stdbool.h>
#endif


        symbol_table* symbol_new(void);
        void symbol_free(symbol_table *symtab);
        void symbol_add(symbol_table *symtab, const char *name, uint32_t addr);
        bool symbol_find_by_addr(symbol_table *symtab, uint32_t addr, const char **ret);
        bool symbol_find_by_addr_near(symbol_table *symtab, uint32_t addr, uint32_t min, uint32_t max, uint32_t *addr_found, const char **ret);
        bool symbol_find_by_name(symbol_table *symtab, const char *name, uint32_t *addr, const char **endret);
        void symbol_list(symbol_table *symtab, struct cpu_debug_t *cpu, debug_outf_t debug_outf);

#ifdef __cplusplus
    }
#endif

#endif
