// Updated monitor.c

#include <stdio.h>
#include <stdlib.h>

// Function prototypes
void check_limits(int soft_limit, int hard_limit);

int main() {
    int soft_limit, hard_limit;

    // Get user input for limits
    printf("Enter soft limit: ");
    scanf("%d", &soft_limit);
    printf("Enter hard limit: ");
    scanf("%d", &hard_limit);

    // Validation: Ensure soft_limit < hard_limit
    if (soft_limit >= hard_limit) {
        fprintf(stderr, "Error: soft limit must be less than hard limit.\n");
        return 1; // Exit with error
    }

    check_limits(soft_limit, hard_limit);
    return 0;
}

void check_limits(int soft_limit, int hard_limit) {
    if (soft_limit < 0 || hard_limit < 0) {
        fprintf(stderr, "Error: Limits must be non-negative.\n");
        return; // Control flow maintained
    }

    // Example processing based on limits
    printf("Soft Limit: %d, Hard Limit: %d\n", soft_limit, hard_limit);
    // Additional code can be added here to utilize these limits
}