// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

lexer grammar StarRocksLex;

ALL: 'ALL';
ALTER: 'ALTER';
AND: 'AND';
ANTI: 'ANTI';
ARRAY: 'ARRAY';
AS: 'AS';
ASC: 'ASC';
BETWEEN: 'BETWEEN';
BUCKETS: 'BUCKETS';
BY: 'BY';
CASE: 'CASE';
CAST: 'CAST';
COLLATE: 'COLLATE';
CONNECTION_ID: 'CONNECTION_ID';
COMMENT: 'COMMENT';
COSTS: 'COSTS';
CREATE: 'CREATE';
CROSS: 'CROSS';
CUBE: 'CUBE';
CURRENT: 'CURRENT';
CURRENT_USER: 'CURRENT_USER';
DATA: 'DATA';
DATABASE: 'DATABASE';
DATABASES: 'DATABASES';
DATE: 'DATE';
DATETIME: 'DATETIME';
DAY: 'DAY';
DECIMAL: 'DECIMAL';
DECIMALV2: 'DECIMALV2';
DECIMAL32: 'DECIMAL32';
DECIMAL64: 'DECIMAL64';
DECIMAL128: 'DECIMAL128';
DEFAULT: 'DEFAULT';
DENSE_RANK: 'DENSE_RANK';
DESC: 'DESC';
DESCRIBE: 'DESCRIBE';
DISTRIBUTED: 'DISTRIBUTED';
DISTINCT: 'DISTINCT';
DUAL: 'DUAL';
ELSE: 'ELSE';
END: 'END';
EXCEPT: 'EXCEPT';
EXISTS: 'EXISTS';
EXPLAIN: 'EXPLAIN';
EXTRACT: 'EXTRACT';
EVERY: 'EVERY';
FALSE: 'FALSE';
FILTER: 'FILTER';
FIRST: 'FIRST';
FIRST_VALUE: 'FIRST_VALUE';
FOLLOWING: 'FOLLOWING';
FOR: 'FOR';
FROM: 'FROM';
FULL: 'FULL';
GLOBAL: 'GLOBAL';
GROUP: 'GROUP';
GROUPING: 'GROUPING';
GROUPING_ID: 'GROUPING_ID';
HASH: 'HASH';
HAVING: 'HAVING';
HOUR: 'HOUR';
IF: 'IF';
IN: 'IN';
INNER: 'INNER';
INSERT: 'INSERT';
INTERSECT: 'INTERSECT';
INTERVAL: 'INTERVAL';
INTO: 'INTO';
IS: 'IS';
JOIN: 'JOIN';
LABEL: 'LABEL';
LAG: 'LAG';
LAST: 'LAST';
LAST_VALUE: 'LAST_VALUE';
LATERAL: 'LATERAL';
LEAD: 'LEAD';
LEFT: 'LEFT';
LESS: 'LESS';
LIKE: 'LIKE';
LIMIT: 'LIMIT';
LOCAL: 'LOCAL';
LOGICAL: 'LOGICAL';
MAXVALUE: 'MAXVALUE';
MINUTE: 'MINUTE';
MINUS: 'MINUS';
MONTH: 'MONTH';
NONE: 'NONE';
NOT: 'NOT';
NULL: 'NULL';
NULLS: 'NULLS';
OFFSET: 'OFFSET';
ON: 'ON';
OR: 'OR';
ORDER: 'ORDER';
OUTER: 'OUTER';
OVER: 'OVER';
PARTITION: 'PARTITION';
PARTITIONS: 'PARTITIONS';
PRECEDING: 'PRECEDING';
PROPERTIES: 'PROPERTIES';
RANGE: 'RANGE';
RANK: 'RANK';
REGEXP: 'REGEXP';
RIGHT: 'RIGHT';
RLIKE: 'RLIKE';
ROLLUP: 'ROLLUP';
ROW: 'ROW';
ROWS: 'ROWS';
ROW_NUMBER: 'ROW_NUMBER';
SCHEMA: 'SCHEMA';
SECOND: 'SECOND';
SELECT: 'SELECT';
SEMI: 'SEMI';
SESSION: 'SESSION';
SET: 'SET';
SETS: 'SETS';
SET_VAR: 'SET_VAR';
SHOW: 'SHOW';
START: 'START';
TABLE: 'TABLE';
TABLES: 'TABLES';
THAN: 'THAN';
THEN: 'THEN';
TIME: 'TIME';
TRUE: 'TRUE';
TYPE: 'TYPE';
UNBOUNDED: 'UNBOUNDED';
UNION: 'UNION';
USE: 'USE';
USER: 'USER';
USING: 'USING';
VARCHAR: 'VARCHAAR';
VALUES: 'VALUES';
VERBOSE: 'VERBOSE';
VIEW: 'VIEW';
WHEN: 'WHEN';
WHERE: 'WHERE';
WITH: 'WITH';
YEAR: 'YEAR';

EQ  : '=';
NEQ : '<>' | '!=';
LT  : '<';
LTE : '<=';
GT  : '>';
GTE : '>=';
EQ_FOR_NULL: '<=>';

PLUS_SYMBOL: '+';
MINUS_SYMBOL: '-';
ASTERISK_SYMBOL: '*';
SLASH_SYMBOL: '/';
PERCENT_SYMBOL: '%';
CONCAT_SYMBOL: '||';

INT_DIV: 'DIV';
BITAND: '&';
BITOR: '|';
BITXOR: '^';
BITNOT: '~';
LOGICAL_NOT: '!';
ARROW: '->';
AT: '@';

SINGLE_QUOTED_TEXT
    : '\'' ( ~'\'' | '\'\'' )* '\''
    ;

DOUBLE_QUOTED_TEXT
    : '"' ( ~'"' | '""' )* '"'
    ;

INTEGER_VALUE
    : DIGIT+
    ;

DECIMAL_VALUE
    : DIGIT+ '.' DIGIT*
    | '.' DIGIT+
    ;

DOUBLE_VALUE
    : DIGIT+ ('.' DIGIT*)? EXPONENT
    | '.' DIGIT+ EXPONENT
    ;

IDENTIFIER
    : (LETTER | '_') (LETTER | DIGIT | '_' | '@' | ':')*
    ;

DIGIT_IDENTIFIER
    : DIGIT (LETTER | DIGIT | '_' | '@' | ':')+
    ;

QUOTED_IDENTIFIER
    : '"' ( ~'"' | '""' )* '"'
    ;

BACKQUOTED_IDENTIFIER
    : '`' ( ~'`' | '``' )* '`'
    ;

fragment EXPONENT
    : 'E' [+-]? DIGIT+
    ;

fragment DIGIT
    : [0-9]
    ;

fragment LETTER
    : [a-zA-Z_$\u0080-\uffff]
    ;

SIMPLE_COMMENT
    : '--' ~[\r\n]* '\r'? '\n'? -> channel(HIDDEN)
    ;

BRACKETED_COMMENT
    : '/*' ~'+' .*? '*/' -> channel(HIDDEN)
    ;

SEMICOLON: ';';

WS
    : [ \r\n\t]+ -> channel(HIDDEN)
    ;