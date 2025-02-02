package com.yugabyte.troubleshoot.ts.models;

import java.util.*;
import lombok.Data;
import lombok.experimental.Accessors;

@Data
@Accessors(chain = true)
public class GraphAnomaly {
  GraphAnomalyType type;
  Long startTime;
  Long endTime;
  Map<String, Set<String>> labels = new HashMap<>();

  public enum GraphAnomalyType {
    INCREASE,
    UNEVEN_DISTRIBUTION
  }

  public GraphAnomaly addLabelsMap(Map<String, String> labels) {
    labels.forEach(
        (key, value) -> this.labels.computeIfAbsent(key, k -> new HashSet<>()).add(value));
    return this;
  }

  public GraphAnomaly merge(GraphAnomaly other) {
    if (startTime != null && other.startTime != null) {
      startTime = Math.min(startTime, other.startTime);
    } else {
      startTime = null;
    }
    if (endTime != null && other.endTime != null) {
      endTime = Math.max(endTime, other.endTime);
    } else {
      endTime = null;
    }
    other.labels.forEach(
        (key, values) -> this.labels.computeIfAbsent(key, k -> new HashSet<>()).addAll(values));
    return this;
  }
}
