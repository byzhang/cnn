#ifndef CNN_EXEC_H
#define CNN_EXEC_H

#include "cnn/cnn.h"

namespace cnn {

class ExecutionEngine {
  friend class ComputationGraph;
 public:
  virtual ~ExecutionEngine();
  virtual void invalidate() = 0;
  virtual const Tensor& forward() = 0;
  virtual const Tensor& forward(VariableIndex i) = 0;
  virtual const Tensor& incremental_forward() = 0;  // if you want to add nodes and evaluate just the new parts
  virtual const Tensor& incremental_forward(VariableIndex i) = 0;
  virtual void set_last_node_evaluated(VariableIndex i) = 0;
  virtual void  set_value(const Tensor& t, VariableIndex i) = 0;
  virtual const Tensor& get_value(VariableIndex i) = 0;
  virtual const Tensor& get_error(VariableIndex i) = 0; 
  virtual void backward() = 0;
 protected:
  explicit ExecutionEngine(const ComputationGraph& cg) : cg(cg) {}
  const ComputationGraph& cg;
};

class SimpleExecutionEngine : public ExecutionEngine {
  friend class ComputationGraph;
public:
  explicit SimpleExecutionEngine(const ComputationGraph& cg) : ExecutionEngine(cg) {}
  void invalidate() override;
  const Tensor& forward() override;
  const Tensor& forward(VariableIndex i) override;
  const Tensor& incremental_forward() override;  // if you want to add nodes and evaluate just the new parts
  const Tensor& incremental_forward(VariableIndex i) override;
  void set_last_node_evaluated(VariableIndex i) override;
  void  set_value(const Tensor& t, VariableIndex i) override;
  const Tensor& get_value(VariableIndex i) override;
  const Tensor& get_error(VariableIndex i) override;
  void backward() override;
private:
  std::vector<Tensor> nfxs;
  std::vector<Tensor> ndEdfs;
  VariableIndex last_node_evaluated;
};

} // namespace cnn

#endif
