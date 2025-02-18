/*
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * ob_expr_format_pico_time.h is for what ...
 *
 * Authors:
 *   Author yaojing <jingfeng.jf@oceanbase.com>
 *
 */
#ifndef _OB_SQL_EXPR_FORMAT_PICO_TIME_H
#define _OB_SQL_EXPR_FORMAT_PICO_TIME_H

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{
class ObExprFormatPicoTime: public ObFuncExprOperator
{
public:
  explicit  ObExprFormatPicoTime(common::ObIAllocator &alloc);
  virtual ~ObExprFormatPicoTime();

  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &type1,
                                common::ObExprTypeCtx &type_ctx) const;
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                       ObExpr &rt_expr) const override;
  static int eval_format_pico_time(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);
  static int eval_format_pico_time_batch(const ObExpr &expr,
                                  ObEvalCtx &ctx,
                                  const ObBitVector &skip,
                                  const int64_t batch_size);
  static int eval_format_pico_time_util(const ObExpr &expr, ObDatum &res_datum,
                                        ObDatum *param1, ObEvalCtx &ctx, int64_t index = 0);
  static const common::ObLength VALUE_BUF_LEN = 20;  //value's string buffer length
  static const common::ObLength LENGTH_FORMAT_PICO_TIME = 11;
private:
  DISALLOW_COPY_AND_ASSIGN(ObExprFormatPicoTime);
  static const uint64_t nano = 1000;
  static const uint64_t micro = 1000 * nano;
  static const uint64_t milli = 1000 * micro;
  static const uint64_t sec = 1000 * milli;
  static const uint64_t min = 60 * sec;
  static const uint64_t hour = 60 * min;
  static const uint64_t day = 24 * hour;
};
}
}
#endif /* _OB_SQL_EXPR_FORMAT_PICO_TIME_H */
