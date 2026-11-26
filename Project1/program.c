/*
 * program.c - Simple Bank Transaction Processing System
 * ------------------------------------------------------
 * Linux Programming Summative - Project 1
 *
 * Simulates a bank account that processes a batch of deposit and withdrawal
 * transactions, then reports total deposits, total withdrawals and the final
 * account balance.
 *
 * The program is intentionally small and deterministic (hard-coded data, no
 * user input) so that its ELF structure, system calls and memory layout can
 * be studied reproducibly with readelf, objdump, strace and gdb.
 *
 * Requirement mapping:
 *   - Exactly three user-defined functions besides main():
 *       create_transactions(), process_transactions(), print_summary()
 *   - Global variables: account_balance (initialised  -> .data section)
 *                       transaction_count (uninitialised -> .bss section)
 *   - struct Transaction: id, amount, type
 *   - calloc() allocates NUM_TRANSACTIONS (6) records on the heap
 *   - for-loop + if/else-if distinguish deposits from withdrawals
 *   - C standard library: printf, calloc, free, perror, exit
 *   - free() releases the heap memory before the program terminates
 *
 * Build (as mandated by the assignment):
 *   gcc -Wall -O0 -fno-inline -o program program.c && strip program
 *   gcc -Wall -O0 -g -fno-inline -o program_debug program.c
 */

#include <stdio.h>
#include <stdlib.h>

#define NUM_TRANSACTIONS 6

#define TYPE_DEPOSIT    'D'
#define TYPE_WITHDRAWAL 'W'

/* One bank transaction record (24 bytes on x86-64: 4 + 4 padding + 8 + 1 + 7 padding). */
struct Transaction {
    int    id;      /* unique transaction ID                   */
    double amount;  /* transaction amount                      */
    char   type;    /* 'D' = deposit, 'W' = withdrawal         */
};

/* ---- Global variables -------------------------------------------------- */
double account_balance = 1000.00;   /* initialised global   -> stored in .data */
int    transaction_count;           /* uninitialised global -> stored in .bss  */

/*
 * create_transactions
 * Allocates an array of n transactions on the heap with calloc() and fills it
 * from constant tables (stored in .rodata). Returns the heap pointer.
 */
struct Transaction *create_transactions(int n)
{
    static const double amounts[NUM_TRANSACTIONS] = {
        500.00, 200.00, 250.75, 1200.50, 900.00, 125.25
    };
    static const char types[NUM_TRANSACTIONS] = {
        TYPE_DEPOSIT, TYPE_WITHDRAWAL, TYPE_DEPOSIT,
        TYPE_WITHDRAWAL, TYPE_WITHDRAWAL, TYPE_DEPOSIT
    };

    struct Transaction *records = calloc(n, sizeof(struct Transaction));
    if (records == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < n; i++) {
        records[i].id     = 1001 + i;
        records[i].amount = amounts[i];
        records[i].type   = types[i];
    }

    transaction_count = n;
    return records;
}

/*
 * process_transactions
 * Loops over every record. Deposits are always applied; withdrawals are only
 * applied when the account holds sufficient funds, otherwise they are rejected.
 * Running totals are returned through the two pointer parameters, and the
 * global account_balance is updated in place.
 */
void process_transactions(struct Transaction *records, int n,
                          double *total_deposits, double *total_withdrawals)
{
    for (int i = 0; i < n; i++) {
        if (records[i].type == TYPE_DEPOSIT) {
            account_balance += records[i].amount;
            *total_deposits += records[i].amount;
            printf("Transaction %d: DEPOSIT    %10.2f  -> balance %10.2f\n",
                   records[i].id, records[i].amount, account_balance);
        } else if (records[i].type == TYPE_WITHDRAWAL) {
            if (records[i].amount <= account_balance) {
                account_balance -= records[i].amount;
                *total_withdrawals += records[i].amount;
                printf("Transaction %d: WITHDRAWAL %10.2f  -> balance %10.2f\n",
                       records[i].id, records[i].amount, account_balance);
            } else {
                printf("Transaction %d: WITHDRAWAL %10.2f  -> REJECTED (insufficient funds)\n",
                       records[i].id, records[i].amount);
            }
        }
    }
}

/*
 * print_summary
 * Displays the totals and the final balance held in the global variable.
 */
void print_summary(double total_deposits, double total_withdrawals)
{
    printf("\n========== Account Summary ==========\n");
    printf("Transactions processed : %d\n", transaction_count);
    printf("Total deposits         : %10.2f\n", total_deposits);
    printf("Total withdrawals      : %10.2f\n", total_withdrawals);
    printf("Final account balance  : %10.2f\n", account_balance);
    printf("=====================================\n");
}

int main(void)
{
    double total_deposits    = 0.0;   /* local variables on main's stack frame */
    double total_withdrawals = 0.0;

    printf("=== Bank Transaction Processing System ===\n");
    printf("Opening balance        : %10.2f\n\n", account_balance);

    struct Transaction *records = create_transactions(NUM_TRANSACTIONS);

    process_transactions(records, NUM_TRANSACTIONS,
                         &total_deposits, &total_withdrawals);

    print_summary(total_deposits, total_withdrawals);

    free(records);       /* release the heap memory obtained from calloc() */
    records = NULL;

    return 0;
}
