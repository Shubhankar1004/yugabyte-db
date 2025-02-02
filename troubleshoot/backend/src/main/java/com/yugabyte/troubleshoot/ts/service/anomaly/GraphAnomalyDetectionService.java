package com.yugabyte.troubleshoot.ts.service.anomaly;

import com.google.common.collect.ImmutableList;
import com.yugabyte.troubleshoot.ts.models.*;
import java.util.*;
import java.util.function.Supplier;
import lombok.AllArgsConstructor;
import lombok.Data;
import lombok.Getter;
import lombok.Value;
import lombok.experimental.Accessors;
import org.apache.commons.collections4.CollectionUtils;
import org.apache.commons.lang3.tuple.MutablePair;
import org.apache.commons.lang3.tuple.Pair;
import org.springframework.stereotype.Service;

@Service
public class GraphAnomalyDetectionService {
  public static final String LINE_NAME = "line_name";

  public List<GraphAnomaly> getAnomalies(
      GraphAnomaly.GraphAnomalyType type, GraphData graphData, AnomalyDetectionSettings settings) {
    return getAnomalies(type, ImmutableList.of(graphData), settings);
  }

  public List<GraphAnomaly> getAnomalies(
      GraphAnomaly.GraphAnomalyType type,
      List<GraphData> graphDatas,
      AnomalyDetectionSettings settings) {
    List<GraphAnomaly> result = new ArrayList<>();
    switch (type) {
      case INCREASE:
        for (GraphData graphData : graphDatas) {
          if (CollectionUtils.isEmpty(graphData.getPoints())) {
            continue;
          }
          GraphData baseline =
              getIncreaseBaseline(graphData, settings.getIncreaseDetectionSettings());
          result.addAll(getIncreases(type, baseline, graphData, settings));
        }
        break;
      case UNEVEN_DISTRIBUTION:
        List<GraphData> baselines = getUnevenDistributionBaselines(graphDatas);
        for (int i = 0; i < graphDatas.size(); i++) {
          GraphData graphData = graphDatas.get(i);
          GraphData baseline = baselines.get(i);
          if (CollectionUtils.isEmpty(graphData.getPoints())) {
            continue;
          }
          result.addAll(getIncreases(type, baseline, graphData, settings));
        }
        break;
      default:
        throw new RuntimeException("Unsupported anomaly type " + type.name());
    }
    return result;
  }

  private List<GraphData> getUnevenDistributionBaselines(List<GraphData> graphDatas) {
    // We're assuming all graphs have the same timestamps, except that
    // some graphs may have less timestamps than the others
    Map<Long, MutablePair<Double, Integer>> timestampToSumCountMap = new HashMap<>();
    graphDatas.stream()
        .flatMap(gd -> gd.getPoints().stream())
        .forEach(
            pt -> {
              if (pt.getY().isInfinite() || pt.getY().isNaN()) {
                return;
              }
              MutablePair<Double, Integer> sumCount = timestampToSumCountMap.get(pt.getX());
              if (sumCount == null) {
                sumCount = new MutablePair<>(pt.getY(), 1);
                timestampToSumCountMap.put(pt.getX(), sumCount);
              } else {
                sumCount.setLeft(sumCount.getLeft() + pt.getY());
                sumCount.setRight(sumCount.getRight() + 1);
              }
            });
    List<GraphData> result = new ArrayList<>();
    for (GraphData graphData : graphDatas) {
      GraphData baseline = new GraphData();
      result.add(baseline);
      for (GraphPoint pt : graphData.getPoints()) {
        Pair<Double, Integer> sumCount = timestampToSumCountMap.get(pt.getX());
        GraphPoint baselinePoint = new GraphPoint().setX(pt.getX()).setY(pt.getY());
        if (sumCount != null
            && sumCount.getRight() > 1
            && !pt.getY().isInfinite()
            && !pt.getY().isNaN()) {
          baselinePoint.setY((sumCount.getLeft() - pt.getY()) / (sumCount.getRight() - 1));
        }
        baseline.getPoints().add(baselinePoint);
      }
    }
    return result;
  }

  private GraphData getIncreaseBaseline(GraphData graphData, IncreaseDetectionSettings settings) {
    List<Double> allPoints = new ArrayList<>();
    int baselineMaxSize = (int) (graphData.getPoints().size() * settings.baselinePointsRatio);
    for (int i = 0; i < graphData.getPoints().size(); i++) {
      Double value = graphData.getPoints().get(i).getY();
      if (value.isNaN() || value.isInfinite()) {
        continue;
      }
      allPoints.add(value);
    }
    if (allPoints.isEmpty()) {
      return null;
    }
    Collections.sort(allPoints);
    List<Double> baselinePoints = allPoints.stream().limit(baselineMaxSize).toList();
    Double average =
        baselinePoints.stream().mapToDouble(Double::doubleValue).sum() / baselinePoints.size();
    return new GraphData()
        .setPoints(
            graphData.getPoints().stream()
                .map(p -> new GraphPoint().setX(p.getX()).setY(average))
                .toList());
  }

  private List<GraphAnomaly> getIncreases(
      GraphAnomaly.GraphAnomalyType anomalyType,
      GraphData beselineGraph,
      GraphData graphData,
      AnomalyDetectionSettings detectionSettings) {
    IncreaseDetectionSettings settings = detectionSettings.getIncreaseDetectionSettings();
    SlidingWindow currentWindow = new SlidingWindow();
    Long currentAnomalyStartTime = null;
    List<GraphAnomaly> result = new ArrayList<>();
    for (int i = 0; i < graphData.getPoints().size(); i++) {
      GraphPoint graphPoint = graphData.getPoints().get(i);
      Double value = graphPoint.getY();
      Double baseline = beselineGraph.getPoints().get(i).getY();
      if (value.isNaN() || value.isInfinite()) {
        // For simplicity, we assume this value as 0 -
        // as that's how we show that on the graph typically;
        value = 0.0;
      }
      Long timestamp = graphPoint.getX();
      if (currentWindow.isEmpty()) {
        if (value > getThreshold(baseline, detectionSettings)) {
          currentWindow.addPoint(timestamp, value, baseline);
        }
        // Until we hit increase threshold - w don't start window.
      } else {
        Long currentWindowStartTime = currentWindow.getStartTime();
        if (timestamp - currentWindowStartTime < settings.windowMinSize) {
          // Just add point to the window
          currentWindow.addPoint(timestamp, value, baseline);
        } else {
          if (currentAnomalyStartTime == null) {
            if (currentWindow.getWindowAverage()
                > getThreshold(currentWindow.getBaselineAverage(), detectionSettings)) {
              // Start an anomaly
              currentAnomalyStartTime = currentWindowStartTime;
              // Continue to grow window until max size is reached.
              currentWindow.addPoint(timestamp, value, baseline);
            } else {
              // Move window further to the next value > threshold
              currentWindow.addPoint(timestamp, value, baseline);
              do {
                currentWindow.removeFirstPoint();
              } while (!currentWindow.isEmpty()
                  && currentWindow.getFirstValue()
                      <= getThreshold(currentWindow.getFirstBaselineValue(), detectionSettings));
            }
          } else {
            if (timestamp - currentWindowStartTime < settings.windowMaxSize) {
              // Just add point to the window
              currentWindow.addPoint(timestamp, value, baseline);
            } else {
              if (currentWindow.getWindowAverage()
                  > getThreshold(currentWindow.getBaselineAverage(), detectionSettings)) {
                // Move window further
                currentWindow.addPointAndMove(timestamp, value, baseline);
              } else {
                // We faced anomaly end.
                // Need to rewind window back to the first point, higher than threshold.
                do {
                  currentWindow.removeLastPoint();
                  i--;
                } while (!currentWindow.isEmpty()
                    && currentWindow.getLastValue()
                        <= getThreshold(currentWindow.getLastBaselineValue(), detectionSettings));
                result.add(
                    fillLabels(
                        new GraphAnomaly()
                            .setType(anomalyType)
                            .setStartTime(currentAnomalyStartTime)
                            .setEndTime(currentWindow.getEndTime()),
                        graphData));
                currentAnomalyStartTime = null;
                currentWindow.clear();
              }
            }
          }
        }
      }
    }
    // Anomaly end is in the end of graph
    if (currentAnomalyStartTime != null) {
      boolean anomalyInProgress = true;
      while (!currentWindow.isEmpty()
          && currentWindow.getLastValue()
              <= getThreshold(currentWindow.getLastBaselineValue(), detectionSettings)) {
        currentWindow.removeLastPoint();
        anomalyInProgress = false;
      }
      result.add(
          fillLabels(
              new GraphAnomaly()
                  .setType(anomalyType)
                  .setStartTime(currentAnomalyStartTime)
                  .setEndTime(
                      anomalyInProgress
                          ? null
                          : currentWindow.isEmpty()
                              ? currentWindow.getLastMovedOutPointTimestamp()
                              : currentWindow.getEndTime()),
              graphData));
    }
    return result;
  }

  private Double getThreshold(Double baseline, AnomalyDetectionSettings settings) {
    double minAnomalyValue =
        settings.getMinimalAnomalyValue() != null
            ? settings.getMinimalAnomalyValue()
            : Double.MIN_VALUE;
    return Math.max(
        baseline * settings.getIncreaseDetectionSettings().thresholdRatio, minAnomalyValue);
  }

  private GraphAnomaly fillLabels(GraphAnomaly anomaly, GraphData graphData) {
    if (graphData.getLabels() != null) {
      anomaly.addLabelsMap(graphData.getLabels());
    }
    Map<String, String> additionalLabels = new HashMap<>();
    if (graphData.getInstanceName() != null) {
      additionalLabels.put(GraphFilter.instanceName.name(), graphData.getInstanceName());
    }
    if (graphData.getName() != null) {
      additionalLabels.put(LINE_NAME, graphData.getName());
    }
    anomaly.addLabelsMap(additionalLabels);
    return anomaly;
  }

  @Getter
  public static class SlidingWindow {
    private final LinkedList<WindowEntry> window = new LinkedList<>();
    private Double windowAverage = 0.0;
    private Double baselineAverage = 0.0;
    private Long lastMovedOutPointTimestamp = null;

    public void addPoint(Long timestamp, Double value, Double baseline) {
      window.add(new WindowEntry(timestamp, value, baseline));
      windowAverage = (windowAverage * (window.size() - 1) + value) / window.size();
      baselineAverage = (baselineAverage * (window.size() - 1) + baseline) / window.size();
    }

    public void addPointAndMove(Long timestamp, Double value, Double baseline) {
      addPoint(timestamp, value, baseline);
      WindowEntry removedPoint = removeFirstPoint();
      if (removedPoint != null) {
        lastMovedOutPointTimestamp = removedPoint.getTimestamp();
      }
    }

    public WindowEntry removeLastPoint() {
      return removePoint(window::pollLast);
    }

    public WindowEntry removeFirstPoint() {
      return removePoint(window::pollFirst);
    }

    public boolean isEmpty() {
      return window.isEmpty();
    }

    public Long getStartTime() {
      if (isEmpty()) {
        return null;
      }
      return window.getFirst().getTimestamp();
    }

    public Long getEndTime() {
      if (isEmpty()) {
        return null;
      }
      return window.getLast().getTimestamp();
    }

    public Double getFirstValue() {
      if (isEmpty()) {
        return null;
      }
      return window.getFirst().getValue();
    }

    public Double getLastValue() {
      if (isEmpty()) {
        return null;
      }
      return window.getLast().getValue();
    }

    public Double getFirstBaselineValue() {
      if (isEmpty()) {
        return null;
      }
      return window.getFirst().getBaseline();
    }

    public Double getLastBaselineValue() {
      if (isEmpty()) {
        return null;
      }
      return window.getLast().getBaseline();
    }

    public void clear() {
      window.clear();
      windowAverage = 0.0;
      baselineAverage = 0.0;
    }

    private WindowEntry removePoint(Supplier<WindowEntry> removeFunction) {
      if (window.isEmpty()) {
        return null;
      }
      WindowEntry removed = removeFunction.get();
      if (window.isEmpty()) {
        windowAverage = 0.0;
        baselineAverage = 0.0;
      } else {
        windowAverage = (windowAverage * (window.size() + 1) - removed.getValue()) / window.size();
        baselineAverage =
            (baselineAverage * (window.size() + 1) - removed.getBaseline()) / window.size();
      }
      return removed;
    }
  }

  public List<GraphAnomaly> mergeAnomalies(List<GraphAnomaly> anomalies) {
    List<GraphAnomaly> sortedAnomalies =
        anomalies.stream().sorted(Comparator.comparing(GraphAnomaly::getStartTime)).toList();

    List<GraphAnomaly> result = new ArrayList<>();
    for (GraphAnomaly anomaly : sortedAnomalies) {
      if (result.isEmpty()) {
        result.add(anomaly);
      } else {
        GraphAnomaly last = result.get(result.size() - 1);
        if (last.getEndTime() == null
            || anomaly.getEndTime() == null
            || last.getEndTime() >= anomaly.getStartTime()) {
          last.merge(anomaly);
        } else {
          result.add(anomaly);
        }
      }
    }
    return result;
  }

  @Value
  @AllArgsConstructor
  private static class WindowEntry {
    long timestamp;
    double value;
    double baseline;
  }

  @Data
  @Accessors(chain = true)
  public static class AnomalyDetectionSettings {
    Double minimalAnomalyValue;
    IncreaseDetectionSettings increaseDetectionSettings = new IncreaseDetectionSettings();
  }

  @Data
  @Accessors(chain = true)
  public static class IncreaseDetectionSettings {
    double baselinePointsRatio = 0.25;
    double thresholdRatio = 1.5;
    long windowMinSize;
    long windowMaxSize;
  }
}
