// Read-only scoped test-account inventory. Never requests secret data or refs.
#import <Foundation/Foundation.h>
#import <Security/Security.h>

int main(void) {
    @autoreleasepool {
        NSMutableArray *services = [NSMutableArray array];
        for (NSString *service in @[@"org.codestudio.Agent.credentials", @"org.codestudio.Agent.oauth"]) {
            NSDictionary *query = @{
                (__bridge id)kSecClass: (__bridge id)kSecClassGenericPassword,
                (__bridge id)kSecAttrService: service,
                (__bridge id)kSecMatchLimit: (__bridge id)kSecMatchLimitAll,
                (__bridge id)kSecReturnAttributes: @YES,
                (__bridge id)kSecReturnData: @NO,
                (__bridge id)kSecReturnRef: @NO,
                (__bridge id)kSecUseAuthenticationUI: (__bridge id)kSecUseAuthenticationUIFail
            };
            CFTypeRef found = NULL;
            OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)query, &found);
            id items = CFBridgingRelease(found);
            NSMutableArray *testAccounts = [NSMutableArray array];
            BOOL valid = status == errSecItemNotFound;
            if (status == errSecSuccess && [items isKindOfClass:[NSArray class]]) {
                valid = YES;
                for (id item in items) {
                    if (![item isKindOfClass:[NSDictionary class]]) { valid = NO; continue; }
                    id account = item[(__bridge id)kSecAttrAccount];
                    id observedService = item[(__bridge id)kSecAttrService];
                    if (![account isKindOfClass:[NSString class]] || ![observedService isEqual:service]) {
                        valid = NO; continue;
                    }
                    if ([account hasPrefix:@"test-credential-"])
                        [testAccounts addObject:@{@"account": account, @"service": service}];
                }
            }
            [testAccounts sortUsingComparator:^NSComparisonResult(id a, id b) {
                return [a[@"account"] compare:b[@"account"]];
            }];
            [services addObject:@{@"service":service, @"osstatus":@(status),
                                  @"inventory_complete":@(valid), @"test_accounts":testAccounts}];
        }
        NSDictionary *result = @{
            @"scope": @"Only generic-password attributes for the two exact CodeStudio services; only test-credential-* accounts emitted.",
            @"secret_data_requested": @NO, @"default_keychain_or_search_list_changed": @NO,
            @"interaction": @"kSecUseAuthenticationUIFail", @"services": services
        };
        NSData *encoded = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        if (!encoded) return 2;
        fwrite(encoded.bytes, 1, encoded.length, stdout);
        fputc('\n', stdout);
        return 0;
    }
}
