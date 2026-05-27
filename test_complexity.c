int no_decision(void) { return 42; }

int one_if(int x) {
  if (x > 0) {
    return 1;
  }
  return 0;
}

int if_else(int x) {
  if (x > 0) {
    return 1;
  } else {
    return 0;
  }
}

int if_elseif_else(int x) {
  if (x > 0) {
    return 1;
  } else if (x < 0) {
    return -1;
  } else {
    return 0;
  }
}

int while_loop(int n) {
  int sum = 0;
  while (n > 0) {
    sum += n;
    n--;
  }
  return sum;
}

int for_loop(int n) {
  int sum = 0;
  for (int i = 0; i < n; i++) {
    sum += i;
  }
  return sum;
}

int do_while_loop(int n) {
  int sum = 0;
  do {
    sum += n;
    n--;
  } while (n > 0);
  return sum;
}

int switch_statement(int x) {
  int result = 0;
  switch (x) {
  case 1:
    result = 10;
    break;
  case 2:
    result = 20;
    break;
  case 3:
    result = 30;
    break;
  default:
    result = -1;
    break;
  }
  return result;
}

int logical_ops(int a, int b) {
  if (a > 0 && b > 0) {
    return 1;
  }
  if (a > 0 || b > 0) {
    return 2;
  }
  return 0;
}

int ternary_op(int x) { return x > 0 ? 1 : (x < 0 ? -1 : 0); }

int complex_function(int x, int n) {
  int sum = 0;
  if (x > 0) {
    for (int i = 0; i < n; i++) {
      if (i % 2 == 0) {
        sum += i;
      } else if (i % 3 == 0) {
        sum += i * 2;
      }
    }
  } else {
    while (n > 0) {
      if (n % 2 == 0 && n > 10) {
        sum += n;
      }
      n--;
    }
  }
  return sum;
}
